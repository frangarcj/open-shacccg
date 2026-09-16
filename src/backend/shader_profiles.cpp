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
    const auto input = code.physical(machine_primary(0), MachineType::F32);
    const auto one = code.physical(machine_special(1), MachineType::F32);
    if (!code.emit<MachineOpcode::Phase>() ||
        !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(3, MachineVectorSwizzle::Source2YYYY),
            code.physical(machine_vertex_output(0),MachineType::F32), input, one) ||
        !code.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(3, MachineVectorSwizzle::Source1OneOneXX),
            code.physical(machine_vertex_output(1),MachineType::F32),
            code.physical(machine_immediate(0),MachineType::F32), one) ||
        !code.emit<MachineOpcode::Emit>()) {
        out.error = "failed to build constructed-position machine program";
        return false;
    }

    uint8_t interface_block[32]{};
    interface_block[0]=0x03; interface_block[16]=0x00; interface_block[17]=0x10;
    interface_block[18]=0x00; interface_block[19]=0x04;
    const gxp::ParameterDesc parameters[] = {
        {position.name.c_str(),0,0,4,0,0,0,1,position.resource_index},
    };

    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190004;
    image.data_buffer_count=0;
    image.primary_phase_count=1;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.parameters=parameters;
    image.parameter_count=1;
    image.primary_register_count=4;
    image.secondary_register_count=0;
    image.compiler_version_raw=0x00033a90;

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
    static constexpr uint8_t src_nums[] = {4,2,0,1};
    for (size_t i=0;i<4;i++) {
        MachineOperand dst;
        uint8_t mask;
        bool no_schedule;
        if (i<2) {
            dst=code.make_value<MachineType::F32>(MachineRegisterClass::VmadAccumulator);
            mask=15; no_schedule=true;
        } else {
            dst=code.physical(machine_vertex_output(static_cast<uint8_t>(i-2)), MachineType::F32);
            mask=3; no_schedule=false;
        }
        if (dst.kind() == MachineOperandKind::None ||
            !code.emit_config<MachineOpcode::Vmad>(static_cast<uint8_t>(i),
                machine_vmad_config(mask, no_schedule), dst,
                code.physical(machine_secondary(src_nums[i]), MachineType::F32), gpi_pair)) {
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
    const gxp::ParameterContainerDesc containers[] = {{14,0,0,16}};
    const gxp::ParameterDesc parameters[] = {
        {position.name.c_str(),0,0,4,0,0,0,1,position.resource_index},
        {varying.name.c_str(),0,0,4,0,0,0,1,varying.resource_index},
        {matrix.name.c_str(),1,0,4,14,0,0,4,0},
    };

    MachineCompileResult compiled;
    if (!compile_words(code,compiled,out,"vertex Machine IR lowering failed")) return false;

    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.data_buffer_count=0;
    image.primary_phase_count=1;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers;
    image.container_count=1;
    image.parameters=parameters;
    image.parameter_count=3;
    image.primary_register_count=8;
    image.secondary_register_count=16;
    image.default_uniform_buffer_count=16;
    image.compiler_version_raw=0x00033a90;

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

bool compile_vertex_uniform_matrix_point_size(const IrAttribute &position,
                                              const IrMatrix4Uniform &matrix,
                                              const IrUniformFloat &point_size,
                                              uint32_t binary_guid, uint32_t source_guid,
                                              IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || position.resource_index!=0 || position.components!=4 ||
        matrix.name.empty() || matrix.resource_index!=0 || point_size.name.empty() ||
        point_size.components!=1 || point_size.resource_index!=16) {
        out.error="uniform-matrix point-size profile requires mat4@0 and scalar point size@16";
        return false;
    }

    MachineProgram primary,secondary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto zero=primary.literal_u32(0);
    if (gpi0.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit_config<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            machine_bitwise_config(true),primary.physical(machine_vertex_output(4),MachineType::U32),
            primary.physical(machine_secondary(16),MachineType::U32),zero) ||
        !primary.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit<MachineOpcode::VmadUniformMat4>(0,
            primary.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !primary.emit<MachineOpcode::Emit>() ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(1),secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(9),MachineType::F32,1)) ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(1),secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(9),MachineType::F32,0)) ||
        !secondary.emit_config<MachineOpcode::Nop>(0,machine_nop_config(true,true))) {
        out.error="failed to build uniform-matrix point-size Machine profile";
        return false;
    }
    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"uniform-matrix point-size primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"uniform-matrix point-size secondary lowering failed"))
        return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x50c10009e0800800ULL,0x40800dbcaf998002ULL,
        0x18903081c011a200ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a2046209ULL,0x08a40086a2045209ULL,0xf804014000000000ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=std::size(expected_secondary) ||
        !std::equal(secondary_compiled.words.begin(),secondary_compiled.words.end(),std::begin(expected_secondary))) {
        out.error="uniform-matrix point-size Machine stream no longer matches Sony oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x11,0,0x05,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={
        {14,0,0,18},{19,0,18,2},
    };
    const gxp::LiteralDesc literals[]={
        {0,0x43ff8000u},{1,0x3f800000u},
    };
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {matrix.name.c_str(),1,0,4,14,0,0,4,0},
        {point_size.name.c_str(),1,0,1,14,0,0,1,16},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=4;
    image.secondary_register_count=20;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=18;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=3;
    image.literals=literals;
    image.literal_count=2;
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected uniform-matrix point-size profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for uniform-matrix point-size profile"; return false;
    }
    return true;
}

bool compile_vertex_uniform_matrix_varying_point_size(const IrAttribute &position,
                                                      const IrAttribute &varying,
                                                      const IrMatrix4Uniform &matrix,
                                                      const IrUniformFloat &point_size,
                                                      IrVaryingSemantic semantic,
                                                      uint32_t binary_guid, uint32_t source_guid,
                                                      IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(varying) || position.resource_index!=0 ||
        varying.resource_index!=4 || position.components!=4 || varying.components!=4 ||
        semantic!=IrVaryingSemantic::Color || matrix.name.empty() || matrix.resource_index!=0 ||
        point_size.name.empty() || point_size.components!=1 || point_size.resource_index!=16) {
        out.error="matrix varying point-size profile requires float4 position/color, mat4@0 and scalar point size@16";
        return false;
    }

    MachineProgram primary,secondary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto zero=primary.literal_u32(0);
    if (gpi0.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4,1,true,true),primary.physical(machine_vertex_output(2),MachineType::F32),
            primary.physical(machine_primary(2),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit<MachineOpcode::VmadUniformMat4>(0,
            primary.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            primary.physical(machine_vertex_output(8),MachineType::U32),
            primary.physical(machine_secondary(16),MachineType::U32),zero) ||
        !primary.emit<MachineOpcode::Emit>() ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(1),secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(9),MachineType::F32,1)) ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(1),secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(8),MachineType::F32),
            secondary.physical(machine_primary(9),MachineType::F32,0)) ||
        !secondary.emit_config<MachineOpcode::Nop>(0,machine_nop_config(true,true))) {
        out.error="failed to build matrix varying point-size Machine profile";
        return false;
    }
    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"matrix varying point-size primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"matrix varying point-size secondary lowering failed"))
        return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x38801d2183080080ULL,0x40800dbcaf998002ULL,
        0x18903081c011a200ULL,0x50810009e1000800ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a2046209ULL,0x08a40086a2045209ULL,0xf804014000000000ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=std::size(expected_secondary) ||
        !std::equal(secondary_compiled.words.begin(),secondary_compiled.words.end(),std::begin(expected_secondary))) {
        out.error="matrix varying point-size Machine stream no longer matches Sony oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0xff,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x09,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,18},{19,0,18,2}};
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {varying.name.c_str(),0,0,4,0,0,0,1,4},
        {matrix.name.c_str(),1,0,4,14,0,0,4,0},
        {point_size.name.c_str(),1,0,1,14,0,0,1,16},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=8;
    image.secondary_register_count=20;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=18;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=4;
    image.literals=literals;
    image.literal_count=2;
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected matrix varying point-size profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for matrix varying point-size profile"; return false;
    }
    return true;
}

bool compile_vertex_uniform_matrix_texcoord_point_size(const IrAttribute &position,
                                                      const IrAttribute &texcoord,
                                                      const IrMatrix4Uniform &position_matrix,
                                                      const IrMatrix4Uniform &texcoord_matrix,
                                                      const IrUniformFloat &point_size,
                                                      uint32_t binary_guid, uint32_t source_guid,
                                                      IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(texcoord) ||
        position.resource_index!=0 || texcoord.resource_index!=4 ||
        position.components!=4 || texcoord.components!=2 ||
        position_matrix.name.empty() || position_matrix.resource_index!=0 ||
        texcoord_matrix.name.empty() || texcoord_matrix.resource_index!=16 ||
        point_size.name.empty() || point_size.components!=1 || point_size.resource_index!=32) {
        out.error="matrix texcoord point-size profile requires position@0, uv@4, mat4@0/16 and scalar point size@32";
        return false;
    }

    MachineProgram primary,secondary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto zero=primary.literal_u32(0);
    if (gpi0.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit_config<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            machine_bitwise_config(true),primary.physical(machine_vertex_output(6),MachineType::U32),
            primary.physical(machine_secondary(24),MachineType::U32),zero) ||
        !primary.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::VmadUniformMat4>(0,machine_vmad_uniform_mat4_config(true),
            primary.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !primary.emit<MachineOpcode::TransformTexcoordMat4XY>(0,
            primary.physical(machine_vertex_output(2),MachineType::F32),
            primary.physical(machine_primary(2),MachineType::F32),
            primary.physical(machine_secondary(8),MachineType::F32)) ||
        !primary.emit<MachineOpcode::Emit>() ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(16),MachineType::F32),
            secondary.physical(machine_primary(17),MachineType::F32,1)) ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(17),MachineType::F32,0)) ||
        !secondary.emit_config<MachineOpcode::Nop>(0,machine_nop_config(true,true))) {
        out.error="failed to build matrix texcoord point-size Machine profile";
        return false;
    }

    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"matrix texcoord point-size primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"matrix texcoord point-size secondary lowering failed"))
        return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x50c10009e0c00c00ULL,0x40800dbcaf998002ULL,
        0x18903881c011a200ULL,0x40800dbcff998812ULL,0x189188818092c202ULL,
        0x40800dbcff998a16ULL,0x189181018092c202ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a3046411ULL,0x08a40086a3045311ULL,0xf804014000000000ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=std::size(expected_secondary) ||
        !std::equal(secondary_compiled.words.begin(),secondary_compiled.words.end(),std::begin(expected_secondary))) {
        out.error="matrix texcoord point-size Machine stream no longer matches Sony oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x3f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x11,0,0x07,0x01,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,34},{19,0,34,2}};
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {texcoord.name.c_str(),0,0,4,0,0,0,1,4},
        {position_matrix.name.c_str(),1,0,4,14,0,0,4,0},
        {texcoord_matrix.name.c_str(),1,0,4,14,0,0,4,16},
        {point_size.name.c_str(),1,0,1,14,0,0,1,32},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=8;
    image.secondary_register_count=36;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=34;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=5;
    image.literals=literals;
    image.literal_count=2;
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected matrix texcoord point-size profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for matrix texcoord point-size profile"; return false;
    }
    return true;
}

bool compile_vertex_uniform_matrix_two_texcoords_color_point_size(
    const IrAttribute &position, const IrAttribute &texcoord0, const IrAttribute &texcoord1,
    const IrAttribute &color, const IrMatrix4Uniform &position_matrix,
    const IrMatrix4Uniform &texcoord_matrix0, const IrMatrix4Uniform &texcoord_matrix1,
    const IrUniformFloat &point_size, uint32_t binary_guid, uint32_t source_guid,
    IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(texcoord0) || !valid_attribute(texcoord1) ||
        !valid_attribute(color) || position.resource_index!=0 || texcoord0.resource_index!=4 ||
        texcoord1.resource_index!=8 || color.resource_index!=12 || position.components!=4 ||
        texcoord0.components!=2 || texcoord1.components!=2 || color.components!=4 ||
        position_matrix.name.empty() || position_matrix.resource_index!=0 ||
        texcoord_matrix0.name.empty() || texcoord_matrix0.name!=texcoord_matrix1.name ||
        texcoord_matrix0.resource_index!=16 || texcoord_matrix1.resource_index!=32 ||
        point_size.name.empty() || point_size.components!=1 || point_size.resource_index!=48) {
        out.error="two-texture matrix profile requires position/uv0/uv1/color, mat4@0, mat4[2]@16 and point size@48";
        return false;
    }

    MachineProgram primary,secondary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto zero=primary.literal_u32(0);
    if (gpi0.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4,1,true,true),primary.physical(machine_vertex_output(2),MachineType::F32),
            primary.physical(machine_primary(6),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::VmadUniformMat4>(0,machine_vmad_uniform_mat4_config(true),
            primary.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !primary.emit_config<MachineOpcode::TransformTexcoordMat4XY>(0,machine_texcoord_mat4_xy_config(true),
            primary.physical(machine_vertex_output(4),MachineType::F32),
            primary.physical(machine_primary(2),MachineType::F32),
            primary.physical(machine_secondary(8),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::TransformTexcoordMat4XY>(0,machine_texcoord_mat4_xy_config(false),
            primary.physical(machine_vertex_output(5),MachineType::F32),
            primary.physical(machine_primary(4),MachineType::F32),
            primary.physical(machine_secondary(16),MachineType::F32)) ||
        !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            primary.physical(machine_vertex_output(12),MachineType::U32),
            primary.physical(machine_secondary(24),MachineType::U32),zero) ||
        !primary.emit<MachineOpcode::Emit>() ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(24),MachineType::F32),
            secondary.physical(machine_primary(25),MachineType::F32,1)) ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(25),MachineType::F32,0)) ||
        !secondary.emit_config<MachineOpcode::Nop>(0,machine_nop_config(true,true))) {
        out.error="failed to build two-texture matrix Machine profile";
        return false;
    }

    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"two-texture matrix primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"two-texture matrix secondary lowering failed"))
        return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x38801d2183080180ULL,0x40800dbcaf998002ULL,
        0x18903881c011a200ULL,0x40800dbcff998812ULL,0x189188818112c202ULL,
        0x40800dbcff998a16ULL,0x189189018112c202ULL,0x40800dbcff999022ULL,
        0x189188818152c204ULL,0x40800dbcff999226ULL,0x189181018152c204ULL,
        0x50810009e1800c00ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a3046619ULL,0x08a40086a3045319ULL,0xf804014000000000ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=std::size(expected_secondary) ||
        !std::equal(secondary_compiled.words.begin(),secondary_compiled.words.end(),std::begin(expected_secondary))) {
        out.error="two-texture matrix Machine stream no longer matches Sony oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x3f,0xf3,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x0d,0x09,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,50},{19,0,50,2}};
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {texcoord0.name.c_str(),0,0,4,0,0,0,1,4},
        {texcoord1.name.c_str(),0,0,4,0,0,0,1,8},
        {color.name.c_str(),0,0,4,0,0,0,1,12},
        {position_matrix.name.c_str(),1,0,4,14,0,0,4,0},
        {texcoord_matrix0.name.c_str(),1,0,4,14,0,0,8,16},
        {point_size.name.c_str(),1,0,1,14,0,0,1,48},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=16;
    image.secondary_register_count=52;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=50;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data();
    image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data();
    image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=7;
    image.literals=literals;
    image.literal_count=2;
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected two-texture matrix profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for two-texture matrix profile"; return false;
    }
    return true;
}

bool compile_vertex_uniform_matrix_three_texcoords_color_point_size(
    const IrAttribute &position, const IrAttribute &texcoord0, const IrAttribute &texcoord1,
    const IrAttribute &texcoord2, const IrAttribute &color, const IrMatrix4Uniform &position_matrix,
    const IrMatrix4Uniform &texcoord_matrix0, const IrMatrix4Uniform &texcoord_matrix1,
    const IrMatrix4Uniform &texcoord_matrix2, const IrUniformFloat &point_size,
    uint32_t binary_guid, uint32_t source_guid, IrCompileResult &out) {
    out={};
    if (!valid_attribute(position) || !valid_attribute(texcoord0) || !valid_attribute(texcoord1) ||
        !valid_attribute(texcoord2) || !valid_attribute(color) || position.resource_index!=0 ||
        texcoord0.resource_index!=4 || texcoord1.resource_index!=8 || texcoord2.resource_index!=12 ||
        color.resource_index!=16 || position.components!=4 || texcoord0.components!=2 ||
        texcoord1.components!=2 || texcoord2.components!=2 || color.components!=4 ||
        position_matrix.name.empty() || position_matrix.resource_index!=0 ||
        texcoord_matrix0.name.empty() || texcoord_matrix0.name!=texcoord_matrix1.name ||
        texcoord_matrix0.name!=texcoord_matrix2.name || texcoord_matrix0.resource_index!=16 ||
        texcoord_matrix1.resource_index!=32 || texcoord_matrix2.resource_index!=48 ||
        point_size.name.empty() || point_size.components!=1 || point_size.resource_index!=64) {
        out.error="three-texture matrix profile requires position/uv0/uv1/uv2/color, mat4@0, mat4[3]@16 and point size@64";
        return false;
    }

    MachineProgram primary,secondary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto zero=primary.literal_u32(0);
    if (gpi0.kind()==MachineOperandKind::None || zero.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() ||
        !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
            machine_move_config(3,4,1,true,true),primary.physical(machine_vertex_output(2),MachineType::F32),
            primary.physical(machine_primary(8),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Pack>(machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0xF,true,false),gpi0,
            primary.physical(machine_primary(0),MachineType::F32),
            primary.physical(machine_primary(1),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::VmadUniformMat4>(0,machine_vmad_uniform_mat4_config(true),
            primary.physical(machine_vertex_output(0),MachineType::F32),gpi0) ||
        !primary.emit_config<MachineOpcode::TransformTexcoordMat4XY>(0,machine_texcoord_mat4_xy_config(true),
            primary.physical(machine_vertex_output(4),MachineType::F32),
            primary.physical(machine_primary(2),MachineType::F32),
            primary.physical(machine_secondary(8),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::TransformTexcoordMat4XY>(0,machine_texcoord_mat4_xy_config(true),
            primary.physical(machine_vertex_output(5),MachineType::F32),
            primary.physical(machine_primary(4),MachineType::F32),
            primary.physical(machine_secondary(16),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::TransformTexcoordMat4XY>(0,machine_texcoord_mat4_xy_config(false),
            primary.physical(machine_vertex_output(6),MachineType::F32),
            primary.physical(machine_primary(6),MachineType::F32),
            primary.physical(machine_secondary(24),MachineType::F32)) ||
        !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
            primary.physical(machine_vertex_output(14),MachineType::U32),
            primary.physical(machine_secondary(24),MachineType::U32),zero) ||
        !primary.emit<MachineOpcode::Emit>() ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(32),MachineType::F32),
            secondary.physical(machine_primary(33),MachineType::F32,1)) ||
        !secondary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(1),secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(12),MachineType::F32),
            secondary.physical(machine_primary(33),MachineType::F32,0)) ||
        !secondary.emit_config<MachineOpcode::Nop>(0,machine_nop_config(true,true))) {
        out.error="failed to build three-texture matrix Machine profile";
        return false;
    }

    MachineCompileResult primary_compiled,secondary_compiled;
    if (!compile_words(primary,primary_compiled,out,"three-texture matrix primary lowering failed") ||
        !compile_words(secondary,secondary_compiled,out,"three-texture matrix secondary lowering failed")) return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x38801d2183080200ULL,0x40800dbcaf998002ULL,
        0x18903881c011a200ULL,0x40800dbcff998812ULL,0x189188818112c202ULL,
        0x40800dbcff998a16ULL,0x189189018112c202ULL,0x40800dbcff999022ULL,
        0x189188818152c204ULL,0x40800dbcff999226ULL,0x189189018152c204ULL,
        0x40800dbcff999832ULL,0x189188818192c206ULL,0x40800dbcff999a36ULL,
        0x189181018192c206ULL,0x50810009e1c00c00ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a3046821ULL,0x08a40086a3045321ULL,0xf804014000000000ULL,
    };
    if (primary_compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(primary_compiled.words.begin(),primary_compiled.words.end(),std::begin(expected_primary)) ||
        secondary_compiled.words.size()!=std::size(expected_secondary) ||
        !std::equal(secondary_compiled.words.begin(),secondary_compiled.words.end(),std::begin(expected_secondary))) {
        out.error="three-texture matrix Machine stream no longer matches Sony oracle words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x3f,0x33,0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x0f,0x49,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,66},{19,0,66,2}};
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),0,0,4,0,0,0,1,0},
        {texcoord0.name.c_str(),0,0,4,0,0,0,1,4},
        {texcoord1.name.c_str(),0,0,4,0,0,0,1,8},
        {texcoord2.name.c_str(),0,0,4,0,0,0,1,12},
        {color.name.c_str(),0,0,4,0,0,0,1,16},
        {position_matrix.name.c_str(),1,0,4,14,0,0,4,0},
        {texcoord_matrix0.name.c_str(),1,0,4,14,0,0,12,16},
        {point_size.name.c_str(),1,0,1,14,0,0,1,64},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=20;
    image.secondary_register_count=68;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=66;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary_compiled.words.data(); image.secondary_instruction_count=secondary_compiled.words.size();
    image.primary_instructions=primary_compiled.words.data(); image.primary_instruction_count=primary_compiled.words.size();
    image.containers=containers; image.container_count=2;
    image.parameters=parameters; image.parameter_count=8;
    image.literals=literals; image.literal_count=2;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected three-texture matrix profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for three-texture matrix profile"; return false;
    }
    return true;
}

bool compile_vertex_fixed16_matrix_machine(const MachineProgram &primary,
                                           const std::vector<IrAttribute> &attributes,
                                           const std::vector<IrMatrix4Uniform> &matrices,
                                           const IrUniformFloat &point_size,
                                           uint32_t binary_guid, uint32_t source_guid,
                                           IrCompileResult &out) {
    out={};
    if (attributes.size()!=3 || matrices.size()!=2 || point_size.components!=1 ||
        attributes[0].components!=4 || attributes[0].resource_index!=0 ||
        attributes[1].components!=2 || attributes[1].resource_index!=4 ||
        attributes[2].components!=4 || attributes[2].resource_index!=8 ||
        matrices[0].resource_index!=0 || matrices[1].resource_index!=16 ||
        point_size.resource_index!=32) {
        out.error="fixed16 matrix profile requires position/uv/color, mat4@0/16 and point-size@32";
        return false;
    }
    (void)primary;

    ProgramBuilder sdk3_primary,secondary;
    auto pack16=[&](usse::RegisterRef dst, usse::RegisterRef src, usse::PackFormat format,
                    uint8_t component, uint8_t repeat, bool scale, bool no_schedule) {
        usse::Vpck16ToF32Semantic pack{};
        pack.dst=dst; pack.src=src; pack.src_format=format; pack.component=component;
        pack.repeat_count=repeat; pack.scale=scale; pack.no_schedule=no_schedule;
        return sdk3_primary.instruction(pack);
    };
    usse::VmovSemantic color{};
    color.dst={usse::RegisterBank::Output,2}; color.src={usse::RegisterBank::PrimaryAttribute,4};
    color.data_type=usse::DataType::F32; color.dest_mask=3; color.swizzle=4; color.repeat_count=1;

    usse::VmovSemantic stage_y{};
    stage_y.dst={usse::RegisterBank::Temp,62}; stage_y.src={usse::RegisterBank::Temp,60};
    stage_y.data_type=usse::DataType::F32; stage_y.dest_mask=2; stage_y.swizzle=0; stage_y.no_schedule=true;

    usse::V32NmadSemantic add_uv{};
    add_uv.op=usse::VectorOp::Add; add_uv.dst={usse::RegisterBank::Temp,60};
    add_uv.src1={usse::RegisterBank::PrimaryAttribute,0}; add_uv.src2={usse::RegisterBank::PrimaryAttribute,5};
    add_uv.dest_mask=2; add_uv.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    add_uv.src2_swizzle=add_uv.src1_swizzle; add_uv.no_schedule=true;

    auto tex_mad=[](uint8_t mask) {
        usse::VmadSemantic mad{};
        mad.dst={usse::RegisterBank::Output,4}; mad.src1={usse::RegisterBank::Temp,60};
        mad.gpi0=1; mad.gpi1=0; mad.write_mask=mask; mad.vec4=true; mad.control_bit_53=false;
        mad.repeat_mode=usse::RepeatMode::Slmsi; mad.no_schedule=true;
        mad.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                           usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
        mad.gpi1_swizzle={{usse::SwizzleChannel::Z,usse::SwizzleChannel::W,
                           usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
        mad.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                           usse::SwizzleChannel::X,usse::SwizzleChannel::Y}};
        return mad;
    };
    usse::VpckSemantic tex_matrix_stage{};
    tex_matrix_stage.dst={usse::RegisterBank::Temp,125};
    tex_matrix_stage.src1={usse::RegisterBank::SecondaryAttribute,10};
    tex_matrix_stage.src2={usse::RegisterBank::SecondaryAttribute,11};
    tex_matrix_stage.src_format=usse::PackFormat::F32; tex_matrix_stage.dst_format=usse::PackFormat::F32;
    tex_matrix_stage.dest_mask=0xf; tex_matrix_stage.no_schedule=true;

    usse::VmovSemantic position_zw{};
    position_zw.dst={usse::RegisterBank::Temp,62}; position_zw.src={usse::RegisterBank::PrimaryAttribute,1};
    position_zw.data_type=usse::DataType::F32; position_zw.dest_mask=0xc; position_zw.swizzle=8;
    position_zw.no_schedule=true;

    usse::V32NmadSemantic position_join{};
    position_join.op=usse::VectorOp::Add; position_join.dst={usse::RegisterBank::Temp,60};
    position_join.src1={usse::RegisterBank::PrimaryAttribute,3}; position_join.src2={usse::RegisterBank::Temp,62};
    position_join.dest_mask=0xf;

    usse::VmadSemantic position_mad{};
    position_mad.dst={usse::RegisterBank::Output,0}; position_mad.src1={usse::RegisterBank::SecondaryAttribute,0};
    position_mad.gpi0=0; position_mad.gpi1=2; position_mad.write_mask=1;
    position_mad.vec4=true; position_mad.control_bit_53=false;
    position_mad.repeat_mode=usse::RepeatMode::External; position_mad.repeat_count=3;
    position_mad.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
    position_mad.gpi1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                usse::SwizzleChannel::Z,usse::SwizzleChannel::Z}};
    position_mad.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                usse::SwizzleChannel::X,usse::SwizzleChannel::Y}};

    usse::VbwSemantic point{};
    point.op=usse::BitwiseOp::Or; point.dst={usse::RegisterBank::Output,10};
    point.src1={usse::RegisterBank::SecondaryAttribute,24}; point.src2_is_immediate=true; point.immediate=0;

    if (!sdk3_primary.phase() || !sdk3_primary.instruction(color) ||
        !pack16({usse::RegisterBank::PrimaryAttribute,6},{usse::RegisterBank::PrimaryAttribute,0},usse::PackFormat::U16,0,3,true,false) ||
        !pack16({usse::RegisterBank::PrimaryAttribute,2},{usse::RegisterBank::PrimaryAttribute,1},usse::PackFormat::S16,1,1,false,true) ||
        !pack16({usse::RegisterBank::Temp,126},{usse::RegisterBank::PrimaryAttribute,0},usse::PackFormat::S16,1,0,false,true) ||
        !pack16({usse::RegisterBank::Temp,124},{usse::RegisterBank::PrimaryAttribute,0},usse::PackFormat::S16,3,0,false,true) ||
        !sdk3_primary.instruction(stage_y) ||
        !pack16({usse::RegisterBank::PrimaryAttribute,0},{usse::RegisterBank::PrimaryAttribute,2},usse::PackFormat::S16,3,0,false,true) ||
        !pack16({usse::RegisterBank::PrimaryAttribute,10},{usse::RegisterBank::PrimaryAttribute,2},usse::PackFormat::U16,2,0,true,true) ||
        !pack16({usse::RegisterBank::Temp,124},{usse::RegisterBank::PrimaryAttribute,2},usse::PackFormat::S16,1,0,false,true) ||
        !pack16({usse::RegisterBank::Temp,125},{usse::RegisterBank::PrimaryAttribute,2},usse::PackFormat::U16,0,0,true,true) ||
        !sdk3_primary.instruction(usse::VdualFixed16AddMoveSemantic{}) ||
        !sdk3_primary.instruction(add_uv) || !sdk3_primary.instruction(tex_mad(1)) ||
        !sdk3_primary.instruction(tex_matrix_stage) || !sdk3_primary.instruction(tex_mad(2)) ||
        !sdk3_primary.instruction(position_zw) || !sdk3_primary.instruction(position_join) ||
        !sdk3_primary.instruction(position_mad) || !sdk3_primary.instruction(point) ||
        !sdk3_primary.emit()) {
        out.error="failed to build SDK 3.0 fixed16 primary stream";
        return false;
    }

    usse::V32NmadSemantic point_max{};
    point_max.op=usse::VectorOp::Max; point_max.dst={usse::RegisterBank::PrimaryAttribute,12};
    point_max.src1={usse::RegisterBank::PrimaryAttribute,16}; point_max.src2={usse::RegisterBank::PrimaryAttribute,17};
    point_max.dest_mask=1;
    point_max.src2_swizzle={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
                             usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
    usse::V32NmadSemantic point_min=point_max;
    point_min.op=usse::VectorOp::Min; point_min.src1={usse::RegisterBank::PrimaryAttribute,12};
    point_min.src2_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,
                             usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    if (!secondary.instruction(point_max) || !secondary.instruction(point_min) || !secondary.nop(false,true)) {
        out.error="failed to build SDK 3.0 fixed16 secondary stream";
        return false;
    }

    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x3880152183080100ULL,0x40813786a0c40000ULL,
        0x40c11986a0400101ULL,0x40c10984afc00001ULL,0x40c10984af800081ULL,
        0x38800d0002f80f00ULL,0x40c10986a0000281ULL,0x40c10786a1440280ULL,
        0x40c10984af800201ULL,0x40c10784afa40200ULL,0x28844000cfb61088ULL,
        0x08800900af001005ULL,0x189188811112c23cULL,0x40c00dbcffb98a16ULL,
        0x189189011112c23cULL,0x38800d408cf80040ULL,0x08a447848f0410feULL,
        0x18903081c011a200ULL,0x50810009e1400c00ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x08a41086a3046411ULL,0x08a40086a3045311ULL,0xf804014000000000ULL,
    };
    if (sdk3_primary.words().size()!=std::size(expected_primary) ||
        !std::equal(sdk3_primary.words().begin(),sdk3_primary.words().end(),std::begin(expected_primary)) ||
        secondary.words().size()!=std::size(expected_secondary) ||
        !std::equal(secondary.words().begin(),secondary.words().end(),std::begin(expected_secondary))) {
        out.error="fixed16 semantic stream no longer matches SDK 3.0 words";
        return false;
    }

    const uint8_t interface_block[32]={
        0x3f,0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x0b,0x01,0,0,0,0x01,0,0,0,0,0,0,0,
    };
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,34},{19,0,34,2}};
    const gxp::ParameterDesc parameters[]={
        {attributes[0].name.c_str(),0,0,4,0,0,0,1,0},
        {attributes[1].name.c_str(),0,0,4,0,0,0,1,4},
        {attributes[2].name.c_str(),0,0,4,0,0,0,1,8},
        {matrices[0].name.c_str(),1,0,4,14,0,0,4,0},
        {matrices[1].name.c_str(),1,0,4,14,0,0,4,16},
        {point_size.name.c_str(),1,0,1,14,0,0,1,32},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00190000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=12;
    image.secondary_register_count=36;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=34;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary.words().data(); image.secondary_instruction_count=secondary.words().size();
    image.primary_instructions=sdk3_primary.words().data(); image.primary_instruction_count=sdk3_primary.words().size();
    image.containers=containers; image.container_count=2;
    image.parameters=parameters; image.parameter_count=std::size(parameters);
    image.literals=literals; image.literal_count=2;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected fixed16 matrix profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for fixed16 matrix profile"; return false;
    }
    return true;
}

bool compile_vertex_matrix_normal_multivarying_point_size(
    const std::vector<IrAttribute> &attributes,
    const IrMatrix4Uniform &modelview, const IrMatrix4Uniform &projection,
    const IrMatrix4Uniform &texcoord_matrix, const IrMatrix3Uniform &normal_matrix,
    const IrUniformFloat &point_size,
    uint32_t binary_guid, uint32_t source_guid, IrCompileResult &out) {
    out={};
    if (attributes.size()!=7 || modelview.name.empty() || projection.name.empty() ||
        texcoord_matrix.name.empty() || normal_matrix.name.empty() || point_size.name.empty() ||
        point_size.components!=1) {
        out.error="matrix-normal multivarying profile requires seven attributes and complete uniform metadata";
        return false;
    }
    const uint8_t expected_components[]={4,2,4,4,4,4,3};
    for (size_t i=0;i<attributes.size();++i) {
        if (!valid_attribute(attributes[i]) || attributes[i].resource_index!=i*4u ||
            attributes[i].components!=expected_components[i]) {
            out.error="matrix-normal multivarying attributes do not match the validated FFP layout";
            return false;
        }
    }

    ProgramBuilder primary;
    if (!primary.phase()) { out.error="failed to emit multivarying PHAS"; return false; }

    auto move=[&](usse::RegisterRef dst, usse::RegisterRef src, uint8_t mask,
                  uint8_t swizzle=4, uint8_t repeat=0) {
        usse::VmovSemantic op{};
        op.dst=dst; op.src=src; op.data_type=usse::DataType::F32;
        op.dest_mask=mask; op.swizzle=swizzle; op.repeat_count=repeat;
        op.skip_invalid=true; op.no_schedule=false;
        return primary.instruction(op);
    };
    auto vop=[&](usse::VectorOp opcode, usse::RegisterRef dst, uint8_t mask,
                 usse::RegisterRef src1, usse::RegisterRef src2,
                 usse::Swizzle4 sw1=usse::Swizzle4{}, usse::Swizzle4 sw2=usse::Swizzle4{}) {
        usse::V32NmadSemantic op{};
        op.op=opcode; op.dst=dst; op.src1=src1; op.src2=src2; op.dest_mask=mask;
        op.src1_swizzle=sw1; op.src2_swizzle=sw2; op.skip_invalid=true;
        return primary.instruction(op);
    };
    const usse::Swizzle4 xxxx={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,
                                 usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    const usse::Swizzle4 yyyy={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
                                 usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
    const usse::Swizzle4 xyz0={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                 usse::SwizzleChannel::Z,usse::SwizzleChannel::Zero}};
    const usse::Swizzle4 xy01={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                 usse::SwizzleChannel::Zero,usse::SwizzleChannel::One}};

    auto mat4=[&](usse::RegisterRef dst, usse::RegisterRef src, uint8_t matrix_sa) {
        for (uint8_t lane=0;lane<4;++lane) {
            if (!vop(usse::VectorOp::Dot,dst,static_cast<uint8_t>(1u<<lane),src,
                     {usse::RegisterBank::SecondaryAttribute,static_cast<uint8_t>(matrix_sa+lane*2u)}))
                return false;
        }
        return true;
    };

    // Fixed POSITION/COLOR outputs and direct material varyings.
    if (!move({usse::RegisterBank::Output,2},{usse::RegisterBank::PrimaryAttribute,4},3,4,1) ||
        !move({usse::RegisterBank::Output,8},{usse::RegisterBank::PrimaryAttribute,6},3,4,1) ||
        !move({usse::RegisterBank::Output,10},{usse::RegisterBank::PrimaryAttribute,8},3,4,1) ||
        !move({usse::RegisterBank::Output,12},{usse::RegisterBank::PrimaryAttribute,10},3,4,1)) {
        out.error="failed to emit multivarying passthrough outputs";
        return false;
    }

    // modelpos = Imodelview * position; POSITION = Jwvp * modelpos.
    if (!mat4({usse::RegisterBank::Temp,40},{usse::RegisterBank::PrimaryAttribute,0},0) ||
        !mat4({usse::RegisterBank::Output,0},{usse::RegisterBank::Temp,40},8)) {
        out.error="failed to emit multivarying model/projection transforms";
        return false;
    }

    // TEXCOORD0 = (Ktexmat * float4(uv,0,1)).xy. Ktexmat starts at word 44 => SA22.
    if (!vop(usse::VectorOp::Dot,{usse::RegisterBank::Output,4},1,
             {usse::RegisterBank::PrimaryAttribute,2},{usse::RegisterBank::SecondaryAttribute,22},xy01) ||
        !vop(usse::VectorOp::Dot,{usse::RegisterBank::Output,4},2,
             {usse::RegisterBank::PrimaryAttribute,2},{usse::RegisterBank::SecondaryAttribute,24},xy01)) {
        out.error="failed to emit multivarying texture transform";
        return false;
    }

    // normal = normalize(Lnormal_mat * Tnormals). A float3x3 occupies three
    // padded float4 columns at words 32,36,40 => SA16,18,20.
    for (uint8_t lane=0;lane<3;++lane) {
        if (!vop(usse::VectorOp::Dot,{usse::RegisterBank::Temp,44},static_cast<uint8_t>(1u<<lane),
                 {usse::RegisterBank::PrimaryAttribute,12},
                 {usse::RegisterBank::SecondaryAttribute,static_cast<uint8_t>(16+lane*2u)},xyz0)) {
            out.error="failed to emit multivarying normal-matrix transform";
            return false;
        }
    }
    if (!vop(usse::VectorOp::Dot,{usse::RegisterBank::Temp,46},1,
             {usse::RegisterBank::Temp,44},{usse::RegisterBank::Temp,44},xyz0)) {
        out.error="failed to emit multivarying normal length";
        return false;
    }
    usse::VcompF32Semantic rsqrt{};
    rsqrt.op=usse::ComplexOp::Rsqrt;
    rsqrt.dst={usse::RegisterBank::Temp,47};
    rsqrt.src={usse::RegisterBank::Temp,46};
    rsqrt.dest_mask=1;
    if (!primary.instruction(rsqrt) ||
        !vop(usse::VectorOp::Mul,{usse::RegisterBank::Temp,48},7,
             {usse::RegisterBank::Temp,44},{usse::RegisterBank::Temp,47},usse::Swizzle4{},xxxx) ||
        !move({usse::RegisterBank::Output,5},{usse::RegisterBank::Temp,48},3,4) ||
        !move({usse::RegisterBank::Output,6},{usse::RegisterBank::Temp,48},1,2)) {
        out.error="failed to emit normalized normal varying";
        return false;
    }

    // ecPosition = modelpos.xyz / modelpos.w. The two float3 varyings are
    // densely packed: normal => O5.xy/O6.x, ecPosition => O6.y/O7.xy.
    usse::VcompF32Semantic reciprocal{};
    reciprocal.op=usse::ComplexOp::Reciprocal;
    reciprocal.dst={usse::RegisterBank::Temp,42};
    reciprocal.src={usse::RegisterBank::Temp,40};
    reciprocal.src_component=3;
    reciprocal.dest_mask=1;
    if (!primary.instruction(reciprocal) ||
        !vop(usse::VectorOp::Mul,{usse::RegisterBank::Temp,50},7,
             {usse::RegisterBank::Temp,40},{usse::RegisterBank::Temp,42},usse::Swizzle4{},xxxx) ||
        !move({usse::RegisterBank::Output,6},{usse::RegisterBank::Temp,50},2,0) ||
        !move({usse::RegisterBank::Output,7},{usse::RegisterBank::Temp,50},1,1) ||
        !move({usse::RegisterBank::Output,7},{usse::RegisterBank::Temp,50},2,2)) {
        out.error="failed to emit eye-space position varying";
        return false;
    }

    // Clamp point size to Sony's observed [1,511] range. Uniform word60 is SA30;
    // literals 511/1 occupy SA31.x/y.
    if (!vop(usse::VectorOp::Max,{usse::RegisterBank::Temp,52},1,
             {usse::RegisterBank::SecondaryAttribute,30},{usse::RegisterBank::SecondaryAttribute,31},xxxx,yyyy) ||
        !vop(usse::VectorOp::Min,{usse::RegisterBank::Temp,52},1,
             {usse::RegisterBank::Temp,52},{usse::RegisterBank::SecondaryAttribute,31},xxxx,xxxx)) {
        out.error="failed to emit multivarying point-size clamp";
        return false;
    }
    usse::VbwSemantic point_copy{};
    point_copy.op=usse::BitwiseOp::Or;
    point_copy.dst={usse::RegisterBank::Output,28};
    point_copy.src1={usse::RegisterBank::Temp,52};
    point_copy.src2_is_immediate=true;
    point_copy.immediate=0;
    if (!primary.instruction(point_copy) || !primary.emit()) {
        out.error="failed to finish multivarying vertex program";
        return false;
    }

    const uint8_t interface_block[32]={
        0x3f,0xff,0xff,0x07,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x1d,0xc1,0xf6,0x1f,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,62},{19,0,62,2}};
    const gxp::LiteralDesc literals[]={{0,0x43ff8000u},{1,0x3f800000u}};
    const gxp::ParameterDesc parameters[]={
        {attributes[0].name.c_str(),0,0,4,0,0,0,1,0},
        {attributes[1].name.c_str(),0,0,4,0,0,0,1,4},
        {attributes[2].name.c_str(),0,0,4,0,0,0,1,8},
        {attributes[3].name.c_str(),0,0,4,0,0,0,1,12},
        {attributes[4].name.c_str(),0,0,4,0,0,0,1,16},
        {attributes[5].name.c_str(),0,0,4,0,0,0,1,20},
        {attributes[6].name.c_str(),0,0,4,0,0,0,1,24},
        {modelview.name.c_str(),1,0,4,14,0,0,4,0},
        {projection.name.c_str(),1,0,4,14,0,0,4,16},
        {texcoord_matrix.name.c_str(),1,0,4,14,0,0,4,44},
        {point_size.name.c_str(),1,0,1,14,0,0,1,60},
        {normal_matrix.name.c_str(),1,0,3,14,0,0,3,32},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00090000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=28;
    image.secondary_register_count=64;
    image.primary_phase_count=1;
    image.data_buffer_count=2;
    image.default_uniform_buffer_count=62;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=primary.words().data(); image.primary_instruction_count=primary.words().size();
    image.containers=containers; image.container_count=2;
    image.parameters=parameters; image.parameter_count=std::size(parameters);
    image.literals=literals; image.literal_count=2;
    image.vertex_primary_padding_word=true;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected matrix-normal multivarying profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for matrix-normal multivarying profile"; return false;
    }
    return true;
}

bool compile_vertex_lighting_machine(const MachineProgram &primary,
                                     const std::vector<IrAttribute> &attributes,
                                     const std::vector<IrUniformFloat> &uniforms,
                                     const std::vector<IrMatrix4Uniform> &matrices,
                                     const IrMatrix3Uniform &normal_matrix,
                                     const std::vector<IrLiteralF32> &literal_values,
                                     uint32_t binary_guid, uint32_t source_guid,
                                     IrCompileResult &out) {
    out={};
    if (attributes.size()!=7 || matrices.size()!=3 || normal_matrix.name.empty()) {
        out.error="lighting vertex profile requires seven attributes, three mat4s and one mat3";
        return false;
    }
    const uint8_t expected_components[]={4,2,4,4,4,4,3};
    uint32_t primary_words=0;
    for (size_t i=0;i<attributes.size();++i) {
        if (!valid_attribute(attributes[i]) || attributes[i].resource_index!=i*4u ||
            attributes[i].components!=expected_components[i]) {
            out.error="lighting vertex attributes do not match the validated FFP layout";
            return false;
        }
        primary_words=std::max(primary_words,attributes[i].resource_index+4u);
    }

    uint32_t uniform_words=normal_matrix.resource_index+12u;
    for (const auto &matrix:matrices) {
        if (matrix.name.empty() || (matrix.resource_index&1u)) {
            out.error="lighting mat4 metadata is invalid";
            return false;
        }
        uniform_words=std::max(uniform_words,matrix.resource_index+16u);
    }
    for (const auto &uniform:uniforms) {
        if (uniform.name.empty() || uniform.components<1 || uniform.components>4) {
            out.error="lighting scalar/vector uniform metadata is invalid";
            return false;
        }
        uniform_words=std::max(uniform_words,uniform.resource_index+uniform.components);
    }
    uniform_words=(uniform_words+1u)&~1u;
    if (uniform_words>0xffffu || uniform_words+literal_values.size()>0xffffu) {
        out.error="lighting secondary-attribute footprint is too large";
        return false;
    }

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"lighting vertex Machine IR lowering failed")) return false;

    std::vector<gxp::ParameterDesc> parameters;
    parameters.reserve(attributes.size()+uniforms.size()+matrices.size()+1);
    for (const auto &attribute:attributes)
        parameters.push_back({attribute.name.c_str(),0,0,4,0,0,0,1,attribute.resource_index});
    for (const auto &matrix:matrices)
        parameters.push_back({matrix.name.c_str(),1,0,4,14,0,0,4,matrix.resource_index});
    parameters.push_back({normal_matrix.name.c_str(),1,0,3,14,0,0,3,normal_matrix.resource_index});
    for (const auto &uniform:uniforms)
        parameters.push_back({uniform.name.c_str(),1,0,uniform.components,14,0,0,1,uniform.resource_index});

    std::vector<gxp::LiteralDesc> literals;
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="lighting literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
    }
    std::vector<gxp::ParameterContainerDesc> containers;
    containers.push_back({14,0,0,static_cast<uint16_t>(uniform_words)});
    if (!literals.empty())
        containers.push_back({19,0,static_cast<uint16_t>(uniform_words),static_cast<uint16_t>(literals.size())});

    // Independently captured from vitaGL's one-light smooth FFP vertex shape.
    // POSITION occupies O0/O1, COLOR O2/O3, TEXCOORD0 O4 and PSIZE O10.
    const uint8_t interface_block[32]={
        0x3f,0xf7,0x77,0x07,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x19,0,0x0b,0x01,0,0,0,0,0,0,0,0,0,0,0,
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00090002;
    image.buffer_flags=0x10000000;
    image.primary_register_count=static_cast<uint16_t>(primary_words);
    image.secondary_register_count=static_cast<uint16_t>(uniform_words+literals.size());
    image.primary_phase_count=1;
    image.data_buffer_count=static_cast<uint32_t>(literals.size());
    image.default_uniform_buffer_count=uniform_words;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
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
    if (!needed) { out.error="GXP writer rejected lighting vertex profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for lighting vertex profile"; return false;
    }
    return true;
}

bool compile_vertex_clip_machine(const MachineProgram &primary,
                                 const std::vector<IrAttribute> &attributes,
                                 const std::vector<IrUniformFloat> &uniforms,
                                 const std::vector<IrMatrix4Uniform> &matrices,
                                 const std::vector<IrLiteralF32> &literal_values,
                                 uint32_t binary_guid, uint32_t source_guid,
                                 IrCompileResult &out) {
    out={};
    if (attributes.size()!=3 || matrices.size()!=3 || uniforms.size()!=2) {
        out.error="clip vertex profile requires three attributes, three mat4s and two uniforms";
        return false;
    }
    const uint8_t expected_components[]={4,2,4};
    for (size_t i=0;i<attributes.size();++i) {
        if (!valid_attribute(attributes[i]) || attributes[i].resource_index!=i*4u ||
            attributes[i].components!=expected_components[i]) {
            out.error="clip vertex attributes do not match the validated FFP layout";
            return false;
        }
    }
    uint32_t uniform_words=0;
    for (const auto &matrix:matrices) {
        if (matrix.name.empty() || (matrix.resource_index&1u)) {
            out.error="clip vertex matrix metadata is invalid";
            return false;
        }
        uniform_words=std::max(uniform_words,matrix.resource_index+16u);
    }
    for (const auto &uniform:uniforms) {
        if (uniform.name.empty() || uniform.components<1 || uniform.components>4) {
            out.error="clip vertex uniform metadata is invalid";
            return false;
        }
        uniform_words=std::max<uint32_t>(uniform_words,uniform.resource_index+uniform.components);
    }
    uniform_words=(uniform_words+1u)&~1u;
    if (uniform_words!=54 || uniform_words+literal_values.size()>0xffffu) {
        out.error="clip vertex profile requires the validated 54-word Open uniform footprint";
        return false;
    }

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"clip vertex Machine IR lowering failed")) return false;

    std::vector<gxp::ParameterDesc> parameters;
    parameters.reserve(attributes.size()+matrices.size()+uniforms.size());
    for (const auto &attribute:attributes)
        parameters.push_back({attribute.name.c_str(),0,0,4,0,0,0,1,attribute.resource_index});
    for (const auto &uniform:uniforms)
        parameters.push_back({uniform.name.c_str(),1,0,uniform.components,14,0,0,1,uniform.resource_index});
    for (const auto &matrix:matrices)
        parameters.push_back({matrix.name.c_str(),1,0,4,14,0,0,4,matrix.resource_index});

    std::vector<gxp::LiteralDesc> literals;
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="clip vertex literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
    }
    std::vector<gxp::ParameterContainerDesc> containers={{14,0,0,static_cast<uint16_t>(uniform_words)}};
    if (!literals.empty())
        containers.push_back({19,0,static_cast<uint16_t>(uniform_words),static_cast<uint16_t>(literals.size())});

    // POSITION + COLOR + TEXCOORD0 + CLP0 + PSIZE. A Sony probe with this
    // exact output mix places CLP0 in O5.y and anchors this interface record.
    const uint8_t interface_block[32]={
        0x3f,0x0f,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0x01,0x19,0,0x0c,0x01,0,0,0,0,0,0,0,0,0,0,0,
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00090002;
    image.buffer_flags=0x10000000;
    image.primary_register_count=12;
    image.secondary_register_count=static_cast<uint16_t>(uniform_words+literals.size());
    image.primary_phase_count=1;
    image.data_buffer_count=static_cast<uint32_t>(literals.size());
    image.default_uniform_buffer_count=uniform_words;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data(); image.container_count=containers.size();
    image.parameters=parameters.data(); image.parameter_count=parameters.size();
    image.literals=literals.data(); image.literal_count=literals.size();
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected clip vertex profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for clip vertex profile"; return false;
    }
    return true;
}

bool compile_vertex_indexed_clear(const IrUniformVec4 &position,
                                  const IrUniformFloat &clear_depth,
                                  uint32_t binary_guid, uint32_t source_guid,
                                  IrCompileResult &out) {
    out={};
    if (position.name.empty() || position.resource_index!=0 || clear_depth.name.empty() ||
        clear_depth.components!=1 || clear_depth.resource_index!=4) {
        out.error="indexed-clear profile requires float4 uniform@0 and scalar depth@4";
        return false;
    }

    ProgramBuilder primary,secondary;
    if (!primary.phase()) { out.error="failed to emit indexed-clear PHAS"; return false; }
    auto compare=[&](usse::Predicate guard,uint8_t predicate,uint8_t literal_sa) {
        usse::VtstSemantic op{};
        op.lhs={usse::RegisterBank::PrimaryAttribute,0};
        op.rhs={usse::RegisterBank::SecondaryAttribute,literal_sa};
        op.predicate=guard;
        op.op=usse::CompareOp::Equal;
        op.predicate_destination=predicate;
        return primary.instruction(op);
    };
    auto copy_u32=[&](uint8_t dst,uint8_t src) {
        usse::VbwSemantic op{};
        op.op=usse::BitwiseOp::Or;
        op.dst={usse::RegisterBank::Output,dst};
        op.src1={usse::RegisterBank::SecondaryAttribute,src};
        op.src2_is_immediate=true;
        op.immediate=0;
        return primary.instruction(op);
    };
    auto move=[&](usse::Predicate predicate,uint8_t dst,usse::RegisterBank src_bank,
                  uint8_t src,uint8_t mask,uint8_t swizzle) {
        usse::VmovSemantic op{};
        op.dst={usse::RegisterBank::Output,dst};
        op.src={src_bank,src};
        op.predicate=predicate;
        op.data_type=usse::DataType::F32;
        op.dest_mask=mask;
        op.swizzle=swizzle;
        return primary.instruction(op);
    };
    if (!compare(usse::Predicate::Always,1,6) ||
        !compare(usse::Predicate::Always,0,8) ||
        !compare(usse::Predicate::NotP0,0,6) ||
        !copy_u32(2,12) ||
        !move(usse::Predicate::NotP0,1,usse::RegisterBank::SecondaryAttribute,1,1,0) ||
        !compare(usse::Predicate::NotP1,1,7) ||
        !copy_u32(0,0) ||
        !move(usse::Predicate::P1,0,usse::RegisterBank::SecondaryAttribute,0,1,1) ||
        !move(usse::Predicate::Always,0,usse::RegisterBank::Output,1,2,0) ||
        !move(usse::Predicate::Always,1,usse::RegisterBank::SecondaryAttribute,5,3,4) ||
        !primary.emit()) {
        out.error="failed to build indexed-clear primary semantic stream";
        return false;
    }

    usse::VmovSemantic depth_tail{};
    depth_tail.dst={usse::RegisterBank::PrimaryAttribute,6};
    depth_tail.src={usse::RegisterBank::PrimaryAttribute,1};
    depth_tail.data_type=usse::DataType::F32;
    depth_tail.dest_mask=1;
    depth_tail.swizzle=1;
    usse::V32NmadSemantic depth_one{};
    depth_one.op=usse::VectorOp::Mul;
    depth_one.dst={usse::RegisterBank::PrimaryAttribute,5};
    depth_one.src1={usse::RegisterBank::PrimaryAttribute,2};
    depth_one.src2={usse::RegisterBank::Special,1};
    depth_one.dest_mask=3;
    depth_one.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::One,
                             usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    depth_one.src2_swizzle={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
                             usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
    depth_one.skip_invalid=true;
    usse::NopSemantic end{};
    end.no_schedule=false;
    end.end=true;
    if (!secondary.instruction(depth_tail) || !secondary.instruction(depth_one) ||
        !secondary.instruction(end)) {
        out.error="failed to build indexed-clear secondary semantic stream";
        return false;
    }

    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0x48880185b007c006ULL,0x48880181b007c008ULL,
        0x4d880181b007c006ULL,0x50810009e0400600ULL,0x3d800501c1040040ULL,
        0x4e880185b007c007ULL,0x50810009e0000000ULL,0x3a800509c1000000ULL,
        0x3880050142000040ULL,0x38800521c3040140ULL,0xfb275000a0200000ULL,
    };
    const uint64_t expected_secondary[]={
        0x3880050a81180040ULL,0x0881118291540081ULL,0xf804014000000000ULL,
    };
    if (primary.words().size()!=std::size(expected_primary) ||
        !std::equal(primary.words().begin(),primary.words().end(),std::begin(expected_primary)) ||
        secondary.words().size()!=std::size(expected_secondary) ||
        !std::equal(secondary.words().begin(),secondary.words().end(),std::begin(expected_secondary))) {
        out.error="indexed-clear semantic stream no longer matches public vitaGL words";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
        0,0x10,0,0x04,0,0,0,0,0,0,0,0,0,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{14,0,0,6},{19,0,6,3}};
    const gxp::LiteralDesc literals[]={{0,2},{1,1},{2,3}};
    const gxp::ParameterDesc parameters[]={
        {position.name.c_str(),1,0,4,14,0,0,1,0},
        {clear_depth.name.c_str(),1,0,1,14,0,0,1,4},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Vertex;
    image.minor_version=5;
    image.sdk_version=0x0350;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x001b0000;
    image.buffer_flags=0x10000000;
    image.primary_register_count=1;
    image.secondary_register_count=13;
    image.primary_phase_count=1;
    image.data_buffer_count=3;
    image.default_uniform_buffer_count=6;
    image.compiler_version_raw=0x00033e40;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.secondary_instructions=secondary.words().data();
    image.secondary_instruction_count=secondary.words().size();
    image.primary_instructions=primary.words().data();
    image.primary_instruction_count=primary.words().size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=2;
    image.literals=literals;
    image.literal_count=3;
    image.vertex_primary_padding_word=true;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected indexed-clear profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for indexed-clear profile"; return false;
    }
    return true;
}

} // namespace vsc::backend

namespace vsc::backend {

bool compile_fragment_linear_fog(const IrUniformVec4 &fog_color,
                                 const IrUniformFloat &fog_range,
                                 const IrUniformFloat &fog_far,
                                 uint32_t binary_guid, uint32_t source_guid,
                                 IrCompileResult &out) {
    out={};
    if (fog_color.name.empty() || fog_color.resource_index!=0 ||
        fog_range.name.empty() || fog_range.components!=1 || fog_range.resource_index!=4 ||
        fog_far.name.empty() || fog_far.components!=1 || fog_far.resource_index!=5) {
        out.error="linear fog profile requires float4@0 plus scalar uniforms @4/@5";
        return false;
    }

    ProgramBuilder secondary,primary;
    usse::VcompF32Semantic sec_rcp{};
    sec_rcp.dst={usse::RegisterBank::PrimaryAttribute,1};
    sec_rcp.src={usse::RegisterBank::PrimaryAttribute,2};
    sec_rcp.src_component=0; sec_rcp.dest_mask=2;
    usse::V32NmadSemantic sec_mul{};
    sec_mul.op=usse::VectorOp::Mul;
    sec_mul.dst={usse::RegisterBank::PrimaryAttribute,2};
    sec_mul.src1={usse::RegisterBank::PrimaryAttribute,2};
    sec_mul.src2={usse::RegisterBank::PrimaryAttribute,1};
    sec_mul.src1_swizzle={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
    sec_mul.src2_swizzle=sec_mul.src1_swizzle;
    sec_mul.dest_mask=1;
    usse::NopSemantic sec_end{}; sec_end.no_schedule=false; sec_end.end=true;
    if (!secondary.instruction(sec_rcp) || !secondary.instruction(sec_mul) || !secondary.instruction(sec_end)) {
        out.error="failed to build SDK 3.0 linear-fog secondary stream";
        return false;
    }

    usse::VcompF32Semantic dist_rcp{};
    dist_rcp.dst={usse::RegisterBank::PrimaryAttribute,2};
    dist_rcp.src={usse::RegisterBank::PrimaryAttribute,3};
    dist_rcp.src_component=1; dist_rcp.dest_mask=1; dist_rcp.no_schedule=true;
    usse::V32NmadSemantic dist_mul{};
    dist_mul.op=usse::VectorOp::Mul;
    dist_mul.dst={usse::RegisterBank::Temp,60};
    dist_mul.src1={usse::RegisterBank::PrimaryAttribute,2};
    dist_mul.src2={usse::RegisterBank::PrimaryAttribute,3};
    dist_mul.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    dist_mul.src2_swizzle=dist_mul.src1_swizzle;
    dist_mul.dest_mask=1; dist_mul.no_schedule=true;
    usse::Vmad2F32ScalarMadSemantic affine{};
    affine.dst={usse::RegisterBank::Temp,60}; affine.src0={usse::RegisterBank::Temp,60};
    affine.src1={usse::RegisterBank::SecondaryAttribute,1}; affine.src2={usse::RegisterBank::SecondaryAttribute,2};
    affine.src1_negative=true; affine.no_schedule=true;
    usse::V32NmadSemantic clamp_min{};
    clamp_min.op=usse::VectorOp::Min; clamp_min.dst={usse::RegisterBank::Temp,60};
    clamp_min.src1={usse::RegisterBank::Temp,60}; clamp_min.src2={usse::RegisterBank::SecondaryAttribute,3};
    clamp_min.dest_mask=1; clamp_min.no_schedule=true;
    clamp_min.src2_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    usse::V32NmadSemantic clamp_max=clamp_min;
    clamp_max.op=usse::VectorOp::Max; clamp_max.src2={usse::RegisterBank::Special,0};
    clamp_max.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::W}};
    usse::VpckSemantic fog_stage{};
    fog_stage.dst={usse::RegisterBank::Temp,125}; fog_stage.src1={usse::RegisterBank::SecondaryAttribute,0};
    fog_stage.src2={usse::RegisterBank::SecondaryAttribute,1}; fog_stage.src_format=usse::PackFormat::F32;
    fog_stage.dst_format=usse::PackFormat::F32; fog_stage.dest_mask=7; fog_stage.components[3]=0;
    usse::VmadSemantic blend0{};
    blend0.dst={usse::RegisterBank::Temp,61}; blend0.src1={usse::RegisterBank::Temp,61};
    blend0.gpi0=0; blend0.gpi1=1; blend0.write_mask=7; blend0.src1_negative=true; blend0.no_schedule=true;
    blend0.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    blend0.gpi1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
    blend0.src1_swizzle=blend0.gpi1_swizzle;
    usse::VmadSemantic blend1{};
    blend1.dst={usse::RegisterBank::PrimaryAttribute,0}; blend1.src1={usse::RegisterBank::PrimaryAttribute,0};
    blend1.gpi0=0; blend1.gpi1=1; blend1.write_mask=3;
    blend1.gpi0_swizzle=blend0.gpi0_swizzle; blend1.gpi1_swizzle=blend0.gpi1_swizzle; blend1.src1_swizzle=blend0.gpi1_swizzle;
    usse::VmadSemantic blend2{};
    blend2.dst={usse::RegisterBank::PrimaryAttribute,1}; blend2.src1={usse::RegisterBank::PrimaryAttribute,1};
    blend2.gpi0=0; blend2.gpi1=1; blend2.write_mask=1; blend2.gpi0_swizzle=blend0.gpi0_swizzle;
    blend2.gpi1_swizzle={{usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,usse::SwizzleChannel::Z}};
    blend2.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    usse::VpckSemantic output{};
    output.dst={usse::RegisterBank::PrimaryAttribute,0}; output.src1={usse::RegisterBank::PrimaryAttribute,0};
    output.src2={usse::RegisterBank::PrimaryAttribute,1}; output.src_format=usse::PackFormat::F32;
    output.dst_format=usse::PackFormat::F16; output.dest_mask=0xf; output.no_schedule=false;
    if (!primary.phase() || !primary.instruction(dist_rcp) || !primary.instruction(dist_mul) ||
        !primary.instruction(affine) || !primary.instruction(clamp_min) || !primary.instruction(clamp_max) ||
        !primary.instruction(fog_stage) || !primary.instruction(blend0) || !primary.instruction(blend1) ||
        !primary.instruction(blend2) || !primary.instruction(output)) {
        out.error="failed to build SDK 3.0 linear-fog primary stream";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,2,0,0,0,
        4,0,0,0,0x0f,0xa0,0xd0,0x0c,0,0,0,0,0x30,0,0,0,
    };
    const uint8_t wpos_input[16]={0,0,0,0,0x0f,0xd0,0xc0,0x0e,0,0,0,0,0x30,0,0,0};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,6},{19,0,6,1}};
    const gxp::ParameterDesc parameters[]={
        {fog_color.name.c_str(),1,0,4,14,0,0,1,0},
        {fog_range.name.c_str(),1,0,1,14,0,0,1,4},
        {fog_far.name.c_str(),1,0,1,14,0,0,1,5},
    };
    const gxp::LiteralDesc literals[]={{0,0x3f800000u}};
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment; image.minor_version=5; image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid; image.program_flags=0x00181000;
    image.buffer_flags=0x10000000; image.primary_register_count=8; image.secondary_register_count=7;
    image.primary_phase_count=1; image.data_buffer_count=1; image.default_uniform_buffer_count=6;
    image.compiler_version_raw=0x00033a90; image.interface_block=interface_block; image.interface_block_size=32;
    image.fragment_additional_inputs=1; image.fragment_input_components=4;
    image.fragment_additional_input_records=wpos_input; image.fragment_additional_input_records_size=16;
    image.secondary_instructions=secondary.words().data(); image.secondary_instruction_count=secondary.words().size();
    image.primary_instructions=primary.words().data(); image.primary_instruction_count=primary.words().size();
    image.containers=containers; image.container_count=2; image.parameters=parameters; image.parameter_count=3;
    image.literals=literals; image.literal_count=1;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected SDK 3.0 linear-fog profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for SDK 3.0 linear-fog profile"; return false;
    }
    return true;
}

bool compile_fragment_exp2_fog(const IrUniformVec4 &fog_color,
                               const IrUniformFloat &fog_density,
                               uint32_t binary_guid, uint32_t source_guid,
                               IrCompileResult &out) {
    out={};
    if (fog_color.name.empty() || fog_color.resource_index!=0 ||
        fog_density.name.empty() || fog_density.components!=1 || fog_density.resource_index!=4) {
        out.error="exp2 fog profile requires float4@0 plus scalar density@4";
        return false;
    }

    ProgramBuilder secondary,primary;
    usse::V32NmadSemantic sec0{};
    sec0.op=usse::VectorOp::Mul; sec0.dst={usse::RegisterBank::PrimaryAttribute,4};
    sec0.src1={usse::RegisterBank::PrimaryAttribute,2}; sec0.src2={usse::RegisterBank::PrimaryAttribute,3};
    sec0.src2_swizzle={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
    sec0.dest_mask=1; sec0.src1_negative=true;
    usse::V32NmadSemantic sec1{};
    sec1.op=usse::VectorOp::Mul; sec1.dst={usse::RegisterBank::PrimaryAttribute,2};
    sec1.src1={usse::RegisterBank::PrimaryAttribute,4}; sec1.src2={usse::RegisterBank::PrimaryAttribute,2};
    sec1.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    sec1.src2_swizzle=sec1.src1_swizzle; sec1.dest_mask=1;
    usse::NopSemantic sec_end{}; sec_end.no_schedule=false; sec_end.end=true;
    if (!secondary.instruction(sec0) || !secondary.instruction(sec1) || !secondary.instruction(sec_end)) {
        out.error="failed to build SDK 3.0 exp2-fog secondary stream"; return false;
    }

    usse::VcompF32Semantic dist_rcp{};
    dist_rcp.dst={usse::RegisterBank::PrimaryAttribute,2}; dist_rcp.src={usse::RegisterBank::PrimaryAttribute,3};
    dist_rcp.src_component=1; dist_rcp.dest_mask=1; dist_rcp.no_schedule=true;
    usse::V32NmadSemantic dist_mul{};
    dist_mul.op=usse::VectorOp::Mul; dist_mul.dst={usse::RegisterBank::Temp,61};
    dist_mul.src1={usse::RegisterBank::PrimaryAttribute,2}; dist_mul.src2={usse::RegisterBank::PrimaryAttribute,3};
    dist_mul.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    dist_mul.src2_swizzle=dist_mul.src1_swizzle; dist_mul.dest_mask=1; dist_mul.no_schedule=true;
    usse::VmadSemantic density_mul{};
    density_mul.dst={usse::RegisterBank::Temp,60}; density_mul.src1={usse::RegisterBank::SecondaryAttribute,2};
    density_mul.gpi0=1; density_mul.gpi1=0; density_mul.write_mask=1; density_mul.vec4=false;
    density_mul.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    density_mul.src1_swizzle=density_mul.gpi0_swizzle; density_mul.gpi1_zero3_extended=true; density_mul.no_schedule=true;
    usse::VdualF32MulMoveSemantic fused{};
    usse::VcompF32Semantic exp2{};
    exp2.op=usse::ComplexOp::Exp2; exp2.dst={usse::RegisterBank::Temp,124}; exp2.src={usse::RegisterBank::Temp,124};
    exp2.dest_mask=1; exp2.no_schedule=true; exp2.exp2_base_dest_type=true;
    usse::V32NmadSemantic clamp_min{};
    clamp_min.op=usse::VectorOp::Min; clamp_min.dst={usse::RegisterBank::Temp,60};
    clamp_min.src1={usse::RegisterBank::Temp,60}; clamp_min.src2={usse::RegisterBank::SecondaryAttribute,3};
    clamp_min.dest_mask=1; clamp_min.no_schedule=true;
    clamp_min.src2_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    usse::V32NmadSemantic clamp_max=clamp_min;
    clamp_max.op=usse::VectorOp::Max; clamp_max.src2={usse::RegisterBank::Special,0};
    clamp_max.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::W}};
    usse::VmadSemantic blend0{};
    blend0.dst={usse::RegisterBank::Temp,61}; blend0.src1={usse::RegisterBank::Temp,61};
    blend0.gpi0=0; blend0.gpi1=1; blend0.write_mask=7; blend0.src1_negative=true; blend0.no_schedule=true;
    blend0.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    blend0.gpi1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
    blend0.src1_swizzle=blend0.gpi1_swizzle;
    usse::VmadSemantic blend1{};
    blend1.dst={usse::RegisterBank::PrimaryAttribute,0}; blend1.src1={usse::RegisterBank::PrimaryAttribute,0};
    blend1.gpi0=0; blend1.gpi1=1; blend1.write_mask=3; blend1.gpi0_swizzle=blend0.gpi0_swizzle;
    blend1.gpi1_swizzle=blend0.gpi1_swizzle; blend1.src1_swizzle=blend0.gpi1_swizzle;
    usse::VmadSemantic blend2{};
    blend2.dst={usse::RegisterBank::PrimaryAttribute,1}; blend2.src1={usse::RegisterBank::PrimaryAttribute,1};
    blend2.gpi0=0; blend2.gpi1=1; blend2.write_mask=1; blend2.gpi0_swizzle=blend0.gpi0_swizzle;
    blend2.gpi1_swizzle={{usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,usse::SwizzleChannel::Z}};
    blend2.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    usse::VpckSemantic output{};
    output.dst={usse::RegisterBank::PrimaryAttribute,0}; output.src1={usse::RegisterBank::PrimaryAttribute,0};
    output.src2={usse::RegisterBank::PrimaryAttribute,1}; output.src_format=usse::PackFormat::F32;
    output.dst_format=usse::PackFormat::F16; output.dest_mask=0xf; output.no_schedule=false;
    if (!primary.phase() || !primary.instruction(dist_rcp) || !primary.instruction(dist_mul) ||
        !primary.instruction(density_mul) || !primary.instruction(fused) || !primary.instruction(exp2) ||
        !primary.instruction(clamp_min) || !primary.instruction(clamp_max) || !primary.instruction(blend0) ||
        !primary.instruction(blend1) || !primary.instruction(blend2) || !primary.instruction(output)) {
        out.error="failed to build SDK 3.0 exp2-fog primary stream"; return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,2,0,0,0,
        4,0,0,0,0x0f,0xa0,0xd0,0x0c,0,0,0,0,0x30,0,0,0,
    };
    const uint8_t wpos_input[16]={0,0,0,0,0x0f,0xd0,0xc0,0x0e,0,0,0,0,0x30,0,0,0};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,6},{19,0,6,2}};
    const gxp::ParameterDesc parameters[]={
        {fog_color.name.c_str(),1,0,4,14,0,0,1,0},
        {fog_density.name.c_str(),1,0,1,14,0,0,1,4},
    };
    const gxp::LiteralDesc literals[]={{0,0x3f800000u},{1,0x40053526u}};
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment; image.minor_version=5; image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid; image.program_flags=0x00181000;
    image.buffer_flags=0x10000000; image.primary_register_count=8; image.secondary_register_count=9;
    image.primary_phase_count=1; image.data_buffer_count=2; image.default_uniform_buffer_count=6;
    image.compiler_version_raw=0x00033a90; image.interface_block=interface_block; image.interface_block_size=32;
    image.fragment_additional_inputs=1; image.fragment_input_components=4;
    image.fragment_additional_input_records=wpos_input; image.fragment_additional_input_records_size=16;
    image.secondary_instructions=secondary.words().data(); image.secondary_instruction_count=secondary.words().size();
    image.primary_instructions=primary.words().data(); image.primary_instruction_count=primary.words().size();
    image.containers=containers; image.container_count=2; image.parameters=parameters; image.parameter_count=2;
    image.literals=literals; image.literal_count=2;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected SDK 3.0 exp2-fog profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for SDK 3.0 exp2-fog profile"; return false;
    }
    return true;
}

bool compile_fragment_srgb(uint32_t binary_guid, uint32_t source_guid,
                           IrCompileResult &out) {
    out={};
    ProgramBuilder primary;
    if (!primary.phase()) { out.error="failed to append sRGB PHAS"; return false; }

    usse::VcompF32Semantic logx{};
    logx.op=usse::ComplexOp::Log2; logx.dst={usse::RegisterBank::Temp,0};
    logx.src={usse::RegisterBank::PrimaryAttribute,0}; logx.src_component=0;
    logx.dest_mask=1; logx.src_absolute=true;
    auto logy=logx; logy.src_component=1; logy.dest_mask=2;
    auto logz=logx; logz.dst={usse::RegisterBank::Temp,1};
    logz.src={usse::RegisterBank::PrimaryAttribute,1}; logz.src_component=0;
    logz.dest_mask=1; logz.no_schedule=true;

    usse::V32NmadSemantic scale_log{};
    scale_log.op=usse::VectorOp::Mul; scale_log.dst={usse::RegisterBank::Temp,61};
    scale_log.src1={usse::RegisterBank::Temp,0}; scale_log.src2={usse::RegisterBank::SecondaryAttribute,2};
    scale_log.src2_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::X,
                             usse::SwizzleChannel::X,usse::SwizzleChannel::X}};
    scale_log.dest_mask=7; scale_log.no_schedule=true;
    usse::VdualF32ExpMoveSemantic dual_exp{};

    usse::VcompF32Semantic expy{};
    expy.op=usse::ComplexOp::Exp2; expy.dst={usse::RegisterBank::Temp,126};
    expy.src={usse::RegisterBank::Temp,125}; expy.src_component=1; expy.dest_mask=2;
    expy.no_schedule=true; expy.exp2_base_dest_type=true;
    auto expz=expy; expz.src_component=2; expz.dest_mask=4;

    const auto sw=[](usse::SwizzleChannel a,usse::SwizzleChannel b,
                     usse::SwizzleChannel c,usse::SwizzleChannel d) {
        return usse::Swizzle4{{a,b,c,d}};
    };
    usse::Vmad2F32Semantic affine_hi{};
    affine_hi.dst={usse::RegisterBank::Temp,61}; affine_hi.src0={usse::RegisterBank::Temp,62};
    affine_hi.src1={usse::RegisterBank::SecondaryAttribute,1}; affine_hi.src2={usse::RegisterBank::SecondaryAttribute,1};
    affine_hi.dest_mask=7; affine_hi.src0_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::W);
    affine_hi.src1_swizzle=sw(usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Y,usse::SwizzleChannel::Y);
    affine_hi.src2_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::X,usse::SwizzleChannel::X); affine_hi.no_schedule=true;

    usse::V32NmadSemantic cutoff_vec{};
    cutoff_vec.op=usse::VectorOp::Add; cutoff_vec.dst={usse::RegisterBank::Temp,62};
    cutoff_vec.src1={usse::RegisterBank::SecondaryAttribute,2}; cutoff_vec.src2={usse::RegisterBank::Temp,0};
    cutoff_vec.src1_swizzle=sw(usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Y,usse::SwizzleChannel::Zero);
    cutoff_vec.src2_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::Y,usse::SwizzleChannel::Z);
    cutoff_vec.src1_negative=true; cutoff_vec.dest_mask=6; cutoff_vec.no_schedule=true;

    usse::VpckSemantic pack_cutoff{};
    pack_cutoff.dst={usse::RegisterBank::Temp,0}; pack_cutoff.src1={usse::RegisterBank::Temp,62};
    pack_cutoff.src2={usse::RegisterBank::Immediate,0}; pack_cutoff.src_format=usse::PackFormat::F32;
    pack_cutoff.dst_format=usse::PackFormat::F32; pack_cutoff.dest_mask=3;
    pack_cutoff.components[0]=2; pack_cutoff.components[1]=1; pack_cutoff.components[2]=0;
    pack_cutoff.components[3]=0; pack_cutoff.no_schedule=true;

    usse::VmovcF32LtZeroSemantic select_xy{};
    select_xy.dst={usse::RegisterBank::Temp,0}; select_xy.test={usse::RegisterBank::Temp,0};
    select_xy.src_true={usse::RegisterBank::SecondaryAttribute,0};
    select_xy.src_false={usse::RegisterBank::Special,0}; select_xy.dest_mask=3; select_xy.no_schedule=true;

    usse::V32NmadSemantic cutoff_z{};
    cutoff_z.op=usse::VectorOp::Add; cutoff_z.dst={usse::RegisterBank::Temp,60};
    cutoff_z.src1={usse::RegisterBank::SecondaryAttribute,2}; cutoff_z.src2={usse::RegisterBank::PrimaryAttribute,0};
    cutoff_z.src1_swizzle=sw(usse::SwizzleChannel::Y,usse::SwizzleChannel::Z,
        usse::SwizzleChannel::W,usse::SwizzleChannel::Zero);
    cutoff_z.src2_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::X,usse::SwizzleChannel::X);
    cutoff_z.src1_negative=true; cutoff_z.dest_mask=1; cutoff_z.no_schedule=true;
    usse::VmovcF32LtZeroSemantic select_z{};
    select_z.dst={usse::RegisterBank::Temp,62}; select_z.test={usse::RegisterBank::Temp,60};
    select_z.src_true={usse::RegisterBank::SecondaryAttribute,0};
    select_z.src_false={usse::RegisterBank::Special,0}; select_z.dest_mask=1; select_z.no_schedule=true;

    usse::Vmad2F32Semantic lower_xy{};
    lower_xy.dst={usse::RegisterBank::PrimaryAttribute,0}; lower_xy.src0={usse::RegisterBank::PrimaryAttribute,0};
    lower_xy.src1={usse::RegisterBank::SecondaryAttribute,0}; lower_xy.src2={usse::RegisterBank::Temp,61};
    lower_xy.dest_mask=3; lower_xy.src0_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::W);
    lower_xy.src1_swizzle=sw(usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Y,usse::SwizzleChannel::Y);
    lower_xy.src2_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::W);
    lower_xy.src2_negative=true; lower_xy.no_schedule=true;
    usse::Vmad2F32Semantic lower_z{};
    lower_z.dst={usse::RegisterBank::Temp,60}; lower_z.src0={usse::RegisterBank::PrimaryAttribute,1};
    lower_z.src1={usse::RegisterBank::SecondaryAttribute,0}; lower_z.src2={usse::RegisterBank::Temp,61};
    lower_z.dest_mask=1; lower_z.src0_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::X,usse::SwizzleChannel::X);
    lower_z.src1_swizzle=sw(usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Y,usse::SwizzleChannel::Y);
    lower_z.src2_swizzle=sw(usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::Z);
    lower_z.src2_negative=true; lower_z.no_schedule=true;

    usse::VmovSemantic merge_cutoff{};
    merge_cutoff.dst={usse::RegisterBank::Temp,62}; merge_cutoff.src={usse::RegisterBank::Temp,0};
    merge_cutoff.data_type=usse::DataType::F32; merge_cutoff.dest_mask=6;
    merge_cutoff.swizzle=8; merge_cutoff.no_schedule=true;

    usse::VmadSemantic mix_xy{};
    mix_xy.dst={usse::RegisterBank::PrimaryAttribute,0}; mix_xy.src1={usse::RegisterBank::PrimaryAttribute,0};
    mix_xy.gpi0=2; mix_xy.gpi1=1; mix_xy.write_mask=3;
    mix_xy.gpi0_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::W);
    mix_xy.gpi1_swizzle=mix_xy.gpi0_swizzle; mix_xy.src1_swizzle=mix_xy.gpi0_swizzle;
    usse::VmadSemantic mix_z{};
    mix_z.dst={usse::RegisterBank::PrimaryAttribute,1}; mix_z.src1={usse::RegisterBank::Temp,60};
    mix_z.gpi0=2; mix_z.gpi1=1; mix_z.write_mask=1;
    mix_z.gpi0_swizzle=sw(usse::SwizzleChannel::Z,usse::SwizzleChannel::Z,
        usse::SwizzleChannel::Z,usse::SwizzleChannel::Z);
    mix_z.gpi1_swizzle=mix_z.gpi0_swizzle;
    mix_z.src1_swizzle=sw(usse::SwizzleChannel::X,usse::SwizzleChannel::X,
        usse::SwizzleChannel::X,usse::SwizzleChannel::X);

    usse::VpckSemantic output{};
    output.dst={usse::RegisterBank::PrimaryAttribute,0}; output.src1={usse::RegisterBank::PrimaryAttribute,0};
    output.src2={usse::RegisterBank::PrimaryAttribute,1}; output.src_format=usse::PackFormat::F32;
    output.dst_format=usse::PackFormat::F16; output.dest_mask=0xf; output.no_schedule=false;

    if (!primary.instruction(logx) || !primary.instruction(logy) || !primary.instruction(logz) ||
        !primary.instruction(scale_log) || !primary.instruction(dual_exp) ||
        !primary.instruction(expy) || !primary.instruction(expz) || !primary.instruction(affine_hi) ||
        !primary.instruction(cutoff_vec) || !primary.instruction(pack_cutoff) ||
        !primary.instruction(select_xy) || !primary.instruction(cutoff_z) ||
        !primary.instruction(select_z) || !primary.instruction(lower_xy) ||
        !primary.instruction(lower_z) || !primary.instruction(merge_cutoff) ||
        !primary.instruction(mix_xy) || !primary.instruction(mix_z) || !primary.instruction(output)) {
        out.error="failed to build SDK 3.0 sRGB primary stream";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,1,0,0,0,
        4,0,0,0,0x0f,0xa0,0xd0,0x0e,0,0,0,0,0x30,0,0,0,
    };
    const gxp::ParameterContainerDesc containers[]={{19,0,0,6}};
    const gxp::LiteralDesc literals[]={
        {0,0x3f800000u},{1,0x414eb852u},{2,0xbd6147aeu},
        {3,0x3f870a3du},{4,0x3ed55555u},{5,0x3b4d2e1cu},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment; image.minor_version=5; image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid; image.program_flags=0x00181000;
    image.primary_register_count=4; image.secondary_register_count=6; image.temp_register_count=3;
    image.primary_phase_count=1; image.data_buffer_count=6; image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block; image.interface_block_size=32;
    image.primary_instructions=primary.words().data(); image.primary_instruction_count=primary.words().size();
    image.containers=containers; image.container_count=1; image.literals=literals; image.literal_count=6;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected SDK 3.0 sRGB profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for SDK 3.0 sRGB profile"; return false;
    }
    return true;
}

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

bool compile_fragment_texture_tint_alpha_discard(const IrUniformFloat &cut,
                                                 const IrUniformVec4 &tint,
                                                 const IrSampler2D &sampler,
                                                 uint32_t binary_guid, uint32_t source_guid,
                                                 IrCompileResult &out) {
    out={};
    if (cut.name.empty() || cut.components!=1 || cut.resource_index!=0 ||
        tint.name.empty() || tint.resource_index!=2 ||
        sampler.name.empty() || sampler.resource_index!=0) {
        out.error="texture-tint alpha-discard profile requires cut@0, float4 tint@2 and sampler0";
        return false;
    }

    ProgramBuilder primary;
    const usse::PhaseSemantic control_phase{usse::PhaseMode::Control};
    const usse::PhaseSemantic main_phase{usse::PhaseMode::Main};
    usse::V32NmadSemantic mul0{};
    mul0.op=usse::VectorOp::Mul;
    mul0.dst={usse::RegisterBank::PrimaryAttribute,0};
    mul0.src1={usse::RegisterBank::SecondaryAttribute,1};
    mul0.src2={usse::RegisterBank::PrimaryAttribute,0};
    mul0.dest_mask=3;
    mul0.skip_invalid=true;
    usse::V32NmadSemantic mul1=mul0;
    mul1.dst.num=1;
    mul1.src1.num=2;
    mul1.src2.num=1;
    mul1.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                        usse::SwizzleChannel::Zero,usse::SwizzleChannel::Zero}};
    usse::VtstF32LaneLessScalarSemantic alpha_test{};
    alpha_test.vector_lane={usse::RegisterBank::PrimaryAttribute,1};
    alpha_test.scalar={usse::RegisterBank::SecondaryAttribute,0};
    alpha_test.predicate_destination=1;
    alpha_test.lane=1;
    usse::KillSemantic kill{};
    kill.predicate=usse::Predicate::P1;
    kill.control_payload=0x306;
    usse::NopSemantic barrier{};
    barrier.no_schedule=false;
    barrier.end=true;
    usse::VpckSemantic output{};
    output.dst={usse::RegisterBank::PrimaryAttribute,0};
    output.src1={usse::RegisterBank::PrimaryAttribute,0};
    output.src2={usse::RegisterBank::PrimaryAttribute,1};
    output.src_format=usse::PackFormat::F32;
    output.dst_format=usse::PackFormat::F16;
    output.dest_mask=0xF;
    output.no_schedule=false;
    output.end=false;
    if (!primary.instruction(control_phase) || !primary.instruction(mul0) ||
        !primary.instruction(mul1) || !primary.instruction(alpha_test) ||
        !primary.instruction(kill) || !primary.instruction(barrier) ||
        !primary.instruction(main_phase) || !primary.instruction(output)) {
        out.error="failed to build texture-tint alpha-discard semantic stream";
        return false;
    }
    const uint64_t expected_primary[]={
        0xfa44010000000000ULL,0x08a44186e0040040ULL,0x08c0418ae0440081ULL,
        0x4888c915b0038080ULL,0xf9300406f0000306ULL,0xf804014000000000ULL,
        0xfa44070000000000ULL,0x40800d7ea0198002ULL,
    };
    if (primary.words().size()!=std::size(expected_primary) ||
        !std::equal(primary.words().begin(),primary.words().end(),std::begin(expected_primary))) {
        out.error="texture-tint alpha-discard semantic stream no longer matches validated words";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,1,0,1,0,
        4,0,0,0,0,0xf9,0,0,0,0,0,0,0xc0,0,0,0,
    };
    const uint8_t extension[8]={0x30,0,0,0,0,0,0,0};
    const uint16_t sampler_query_info[16]={0x0301};
    const gxp::ParameterContainerDesc containers[]={{14,0,0,6},{19,0,6,1}};
    const gxp::LiteralDesc literals[]={{0,0x0000e000u}};
    const gxp::ParameterDesc parameters[]={
        {cut.name.c_str(),1,0,1,14,0,0,1,0},
        {tint.name.c_str(),1,0,4,14,0,0,1,2},
        {sampler.name.c_str(),2,0,4,0,1,0,1,0},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid;
    image.source_guid=source_guid;
    image.program_flags=0x00180809;
    image.buffer_flags=0x10000000;
    image.texunit_flags[0]=1;
    image.primary_register_count=4;
    image.secondary_register_count=7;
    image.primary_phase_count=2;
    image.data_buffer_count=1;
    image.default_uniform_buffer_count=6;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.fragment_interface_extension=extension;
    image.fragment_interface_extension_size=sizeof(extension);
    image.fragment_primary_prefix_word=6;
    image.sampler_query_info=sampler_query_info;
    image.sampler_query_info_count=16;
    image.primary_instructions=primary.words().data();
    image.primary_instruction_count=primary.words().size();
    image.containers=containers;
    image.container_count=2;
    image.parameters=parameters;
    image.parameter_count=3;
    image.literals=literals;
    image.literal_count=1;

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected texture-tint alpha-discard profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for texture-tint alpha-discard profile"; return false;
    }
    return true;
}

bool compile_fragment_two_texture_combine(const IrSampler2D &sampler0,
                                          const IrSampler2D &sampler1,
                                          uint32_t binary_guid, uint32_t source_guid,
                                          IrCompileResult &out) {
    out={};
    if (sampler0.name.empty() || sampler1.name.empty() || sampler0.resource_index!=1 ||
        sampler1.resource_index!=2) {
        out.error="two-texture combine profile requires samplers at TEXUNIT1/2";
        return false;
    }

    MachineProgram primary;
    const auto gpi0=primary.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
    const auto gpi_pair=primary.pair(gpi0,gpi0);
    const auto temp60=primary.physical(usse::RegisterBank::Temp,60,MachineType::F32);
    if (gpi0.kind()==MachineOperandKind::None || gpi_pair.kind()==MachineOperandKind::None ||
        !primary.emit<MachineOpcode::Phase>() || !primary.emit<MachineOpcode::Nop>() ||
        !primary.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
            machine_pack_config(0x7),gpi0,
            primary.physical(machine_primary(2),MachineType::F32),
            primary.physical(machine_primary(3),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Vmad>(0,
            machine_vmad_config(0x7,true,true,true),temp60,
            primary.physical(machine_primary(0),MachineType::F32),gpi_pair) ||
        !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
            machine_vector_config(0x7,MachineVectorSwizzle::Source2YYYY,false,false,false,true,true),
            temp60,temp60,primary.physical(machine_special(1),MachineType::F32)) ||
        !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
            machine_vector_config(0x7,MachineVectorSwizzle::Identity,false,false,false,true,true),
            temp60,temp60,primary.physical(machine_special(0),MachineType::F32,0)) ||
        !primary.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
            machine_vector_config(0x8),temp60,
            primary.physical(machine_primary(3),MachineType::F32,1),
            primary.physical(machine_primary(1),MachineType::F32,1)) ||
        !primary.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
            machine_pack_config(0xF,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16),temp60,
            primary.physical(machine_immediate(0),MachineType::F32))) {
        out.error="failed to build two-texture combine Machine program";
        return false;
    }
    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"two-texture combine Machine lowering failed")) return false;
    const uint64_t expected_primary[]={
        0xfa44070000000000ULL,0xf800094000000000ULL,0x40c00d9caf818206ULL,
        0x18a18b848f1d0100ULL,0x08a51b841f045f01ULL,0x08a50b841f046f00ULL,
        0x08841400af2480c1ULL,0x40810d7e2019bc00ULL,
    };
    if (compiled.words.size()!=std::size(expected_primary) ||
        !std::equal(compiled.words.begin(),compiled.words.end(),std::begin(expected_primary))) {
        out.error="two-texture combine Machine stream no longer matches SDK 3.0 words";
        return false;
    }

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,2,0,2,0,
        4,0,0,0,1,0xf1,0,0,1,0,0,0,0xc0,0,0,0,
    };
    const uint8_t additional_input[16]={
        0x30,0,0,0,0x02,0xf9,0,0,0x02,0,0,0,0xc0,0,0,0,
    };
    const uint8_t extension[8]={0x30,0,0,0,0,0,0,0};
    const gxp::ParameterDesc parameters[]={
        {sampler0.name.c_str(),2,0,4,0,1,0,1,1},
        {sampler1.name.c_str(),2,0,4,0,1,0,1,2},
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.minor_version=5;
    image.sdk_version=0x0300;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00180801;
    image.texunit_flags[0]=0x00000110;
    image.primary_register_count=8;
    image.secondary_register_count=0;
    image.primary_phase_count=1;
    image.data_buffer_count=0;
    image.compiler_version_raw=0x00033a90;
    image.interface_block=interface_block; image.interface_block_size=sizeof(interface_block);
    image.fragment_additional_inputs=1;
    image.fragment_input_components=4;
    image.fragment_additional_input_records=additional_input;
    image.fragment_additional_input_records_size=sizeof(additional_input);
    image.fragment_interface_extension=extension;
    image.fragment_interface_extension_size=sizeof(extension);
    std::array<uint16_t,16> sampler_query_info{};
    sampler_query_info[1]=0x0301;
    sampler_query_info[2]=0x0301;
    image.sampler_query_info=sampler_query_info.data();
    image.sampler_query_info_count=sampler_query_info.size();
    image.primary_instructions=compiled.words.data(); image.primary_instruction_count=compiled.words.size();
    image.parameters=parameters; image.parameter_count=2;
    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected two-texture combine profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for two-texture combine profile"; return false;
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

bool compile_fragment_lighting_machine(const MachineProgram &primary,
                                       const std::vector<IrUniformFloat> &uniforms,
                                       const std::vector<IrLiteralF32> &literal_values,
                                       uint32_t binary_guid, uint32_t source_guid,
                                       IrCompileResult &out) {
    out={};
    uint32_t uniform_words=0;
    for (const auto &uniform:uniforms) {
        if (uniform.name.empty() || uniform.components<1 || uniform.components>4) {
            out.error="fragment lighting uniform metadata is invalid";
            return false;
        }
        uniform_words=std::max<uint32_t>(uniform_words,uniform.resource_index+uniform.components);
    }
    uniform_words=(uniform_words+1u)&~1u;
    if (uniform_words!=26 || uniform_words+literal_values.size()>0xffffu) {
        out.error="fragment lighting profile requires the validated 26-word uniform footprint";
        return false;
    }

    MachineCompileResult compiled;
    if (!compile_words(primary,compiled,out,"fragment lighting Machine IR lowering failed")) return false;

    std::vector<gxp::ParameterDesc> parameters;
    parameters.reserve(uniforms.size());
    for (const auto &uniform:uniforms)
        parameters.push_back({uniform.name.c_str(),1,0,uniform.components,14,0,0,1,uniform.resource_index});
    std::vector<gxp::LiteralDesc> literals;
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="fragment lighting literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
    }
    std::vector<gxp::ParameterContainerDesc> containers={{14,0,0,static_cast<uint16_t>(uniform_words)}};
    if (!literals.empty())
        containers.push_back({19,0,static_cast<uint16_t>(uniform_words),static_cast<uint16_t>(literals.size())});

    const uint8_t interface_block[32]={
        0,0,0,0,0,0,0,0,0,0,1,4,6,0,0,0,
        4,0,0,0,0x0f,0x20,0xc0,0x0c,0,0,0,0,0x30,0,0,0,
    };
    const uint8_t additional_inputs[5*16]={
        0,0,0,0,0x0f,0x30,0xc0,0x0c,0,0,0,0,0x30,0,0,0,
        0,0,0,0,0x0f,0x40,0xc0,0x0c,0,0,0,0,0x30,0,0,0,
        0,0,0,0,0x0f,0x50,0xc0,0x0c,0,0,0,0,0x30,0,0,0,
        0,0,0,0,0x0f,0x60,0xc0,0x0c,0,0,0,0,0x30,0,0,0,
        0,0,0,0,0x0f,0xa0,0xd0,0x0e,0,0,0,0,0x30,0,0,0,
    };
    gxp::ProgramImage image{};
    image.type=gxp::ProgramType::Fragment;
    image.sdk_version=0x0165;
    image.binary_guid=binary_guid; image.source_guid=source_guid;
    image.program_flags=0x00081007;
    image.buffer_flags=0x10000000;
    image.primary_register_count=24;
    image.secondary_register_count=static_cast<uint16_t>(uniform_words+literals.size());
    image.temp_register_count=9;
    image.primary_phase_count=1;
    image.data_buffer_count=static_cast<uint32_t>(literals.size());
    image.default_uniform_buffer_count=uniform_words;
    image.compiler_version_raw=0x0002df30;
    image.interface_block=interface_block;
    image.interface_block_size=sizeof(interface_block);
    image.fragment_additional_inputs=5;
    image.fragment_input_components=4;
    image.fragment_additional_input_records=additional_inputs;
    image.fragment_additional_input_records_size=sizeof(additional_inputs);
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data(); image.container_count=containers.size();
    image.parameters=parameters.data(); image.parameter_count=parameters.size();
    image.literals=literals.data(); image.literal_count=literals.size();

    const size_t needed=gxp::required_size(image);
    if (!needed) { out.error="GXP writer rejected fragment lighting profile"; return false; }
    out.gxp.resize(needed);
    if (!gxp::write_program(image,out.gxp.data(),out.gxp.size())) {
        out.gxp.clear(); out.error="GXP writer failed for fragment lighting profile"; return false;
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
    std::array<uint16_t,16> sampler_query_info{};
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
        if (!primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                machine_pack_config(0xF, true, false),
                primary.physical(machine_fragment_output(0), MachineType::F16),
                primary.physical(machine_secondary(0), MachineType::F32),
                primary.physical(machine_secondary(1), MachineType::F32))) {
            out.error = "failed to build uniform-color fragment Machine IR";
            return false;
        }
        interface_block[10] = 1; interface_block[11] = 4; interface_block[16] = 4;
        containers = {{14,0,0,4}};
        parameters.push_back({uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        image.minor_version=5;
        image.sdk_version=0x0300;
        image.program_flags=0x00180000;
        image.buffer_flags = 0x10000000;
        image.primary_register_count = 2;
        image.secondary_register_count = 4;
        image.data_buffer_count=0;
        image.default_uniform_buffer_count = 4;
        image.compiler_version_raw = 0x00033a90;
        image.fragment_primary_overlaps_interface=true;
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
        image.minor_version=5;
        image.sdk_version=0x0300;
        image.program_flags=0x00181000;
        image.primary_register_count=4;
        image.secondary_register_count=0;
        image.data_buffer_count=0;
        image.compiler_version_raw=0x00033a90;
        break;

    case FragmentMachineProfile::TextureTint2D: {
        if (uniforms.size()!=1 || samplers.size()!=1 || uniforms[0].name.empty() ||
            samplers[0].name.empty() || uniforms[0].resource_index!=0 || samplers[0].resource_index!=0) {
            out.error="texture-tint fragment profile requires one float4 uniform and one sampler2D at resource 0";
            return false;
        }
        if (!primary.emit<MachineOpcode::Nop>() ||
            !primary.emit<MachineOpcode::MulPackF32>(4,
                primary.physical(machine_fragment_output(0),MachineType::F16),
                primary.physical(machine_secondary(0),MachineType::F32),
                primary.physical(machine_primary(0),MachineType::F32))) {
            out.error="failed to build texture-tint Machine IR";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=0x00; interface_block[21]=samplers[0].point_coord ? 0xfd : 0xf9; interface_block[28]=0xc0;
        fragment_extension[0]=0x30;
        image.fragment_interface_extension=fragment_extension;
        image.fragment_interface_extension_size=sizeof(fragment_extension);
        containers={{14,0,0,4}};
        parameters.push_back({uniforms[0].name.c_str(),1,0,4,14,0,0,1,0});
        parameters.push_back({samplers[0].name.c_str(),2,0,4,0,1,0,1,0});
        sampler_query_info[0]=0x0301;
        image.minor_version=5;
        image.sdk_version=0x0300;
        image.program_flags=0x00180804 | (samplers[0].point_coord ? 0x20u : 0u);
        image.buffer_flags=0x10000000;
        image.texunit_flags[0]=1;
        image.primary_register_count=4;
        image.secondary_register_count=4;
        image.data_buffer_count=0;
        image.default_uniform_buffer_count=4;
        image.compiler_version_raw=0x00033a90;
        image.sampler_query_info=sampler_query_info.data();
        image.sampler_query_info_count=sampler_query_info.size();
        break;
    }

    case FragmentMachineProfile::Texture2D:
        if (!uniforms.empty() || samplers.size()!=1 || samplers[0].name.empty() ||
            samplers[0].resource_index>1 || samplers[0].texcoord_index>1) {
            out.error="texture fragment profile requires sampler2D/TEXCOORD index 0 or 1";
            return false;
        }
        interface_block[10]=1; interface_block[11]=4; interface_block[12]=1; interface_block[14]=1; interface_block[16]=4;
        interface_block[20]=samplers[0].texcoord_index; interface_block[21]=0xf9;
        interface_block[24]=static_cast<uint8_t>(samplers[0].resource_index);
        interface_block[28]=0x40;
        fragment_extension[0]=0x20;
        image.fragment_interface_extension=fragment_extension;
        image.fragment_interface_extension_size=sizeof(fragment_extension);
        parameters.push_back({samplers[0].name.c_str(),2,0,4,0,2,0,1,samplers[0].resource_index});
        sampler_query_info[samplers[0].resource_index]=0x0302;
        image.minor_version=5;
        image.sdk_version=0x0300;
        image.program_flags=0x00180800;
        image.texunit_flags[0]=1u<<(4u*samplers[0].resource_index);
        image.primary_register_count=2;
        image.secondary_register_count=0;
        image.data_buffer_count=0;
        image.compiler_version_raw=0x00033a90;
        image.sampler_query_info=sampler_query_info.data();
        image.sampler_query_info_count=sampler_query_info.size();
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
                                         const std::vector<IrUniformFloat> &uniforms,
                                         const std::vector<IrLiteralF32> &literal_values,
                                         uint8_t float_input_count,
                                         uint8_t float_components,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out) {
    out = {};
    if (float_input_count < 1 || float_input_count > 3 || float_components < 1 || float_components > 4) {
        out.error="fragment arithmetic profile currently covers one to three F32 inputs of width 1..4";
        return false;
    }
    const bool classic_uniform_profile=!uniforms.empty() && float_input_count==1 && float_components==4 &&
        std::all_of(uniforms.begin(),uniforms.end(),[](const IrUniformFloat &u) { return u.components==4; });
    if (!uniforms.empty() && float_components!=4) {
        out.error="uniform arithmetic profile currently requires float4 output/input width";
        return false;
    }
    uint8_t interface_block[32]{};
    std::vector<gxp::ParameterContainerDesc> containers;
    std::vector<gxp::ParameterDesc> parameters;
    std::vector<gxp::LiteralDesc> literals;

    const uint8_t component_code=float_components==1 ? 0x00 : (float_components==2 ? 0x40 : 0xc0);
    const uint8_t component_tail=float_components==1 ? 0x00 : (float_components==2 ? 0x10 : 0x30);
    interface_block[10]=1; interface_block[11]=4; interface_block[12]=float_input_count; interface_block[16]=4;
    interface_block[20]=0x0f;
    if (uniforms.empty()) {
        interface_block[22]=component_code;
        interface_block[23]=float_input_count==1 ? 0x0e : 0x0c;
        interface_block[28]=component_tail;
        if (!literal_values.empty())
            containers.push_back({19,0,0,static_cast<uint16_t>(literal_values.size())});
    } else if (classic_uniform_profile) {
        interface_block[21]=0xa0; interface_block[22]=0xd0; interface_block[23]=0x0e;
        interface_block[28]=0xb0;
        containers.push_back({19,0,0,2});
        containers.insert(containers.begin(),{14,0,0,static_cast<uint16_t>(4*uniforms.size())});
    } else {
        interface_block[21]=0xa0; interface_block[22]=0xd0;
        interface_block[23]=float_input_count==1 ? 0x0e : 0x0c;
        interface_block[28]=0x30;
        uint32_t uniform_words=0;
        for (const auto &uniform:uniforms) {
            if (uniform.name.empty() || uniform.components<1 || uniform.components>4 ||
                uniform.resource_index<uniform_words) {
                out.error="mixed arithmetic uniform metadata is invalid or overlapping";
                return false;
            }
            uniform_words=std::max<uint32_t>(uniform_words,uniform.resource_index+uniform.components);
        }
        uniform_words=(uniform_words+1u)&~1u;
        if (uniform_words>0xffffu) { out.error="mixed arithmetic uniform footprint is too large"; return false; }
        containers.push_back({14,0,0,static_cast<uint16_t>(uniform_words)});
        if (!literal_values.empty())
            containers.push_back({19,0,static_cast<uint16_t>(uniform_words),static_cast<uint16_t>(literal_values.size())});
    }
    for(size_t i=0;i<uniforms.size();++i) {
        if(uniforms[i].name.empty()) { out.error="arithmetic uniform name cannot be empty"; return false; }
        if (classic_uniform_profile && uniforms[i].resource_index!=i*4u) {
            out.error="classic arithmetic float4 uniforms must use contiguous 4-word offsets";
            return false;
        }
        parameters.push_back({uniforms[i].name.c_str(),1,0,uniforms[i].components,14,0,0,1,uniforms[i].resource_index});
    }
    literals.reserve(literal_values.size());
    for (const auto &literal:literal_values) {
        if (literal.resource_index>=literal_values.size()) {
            out.error="arithmetic literal resource index is out of range";
            return false;
        }
        literals.push_back({literal.resource_index,literal.value_bits});
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
        image.secondary_register_count=static_cast<uint16_t>(literal_values.size());
        image.data_buffer_count=static_cast<uint32_t>(literal_values.size());
        image.compiler_version_raw=0x0002df30;
    } else if (classic_uniform_profile) {
        image.program_flags=0x1000;
        image.primary_register_count=4;
        image.secondary_register_count=static_cast<uint16_t>(2 + uniforms.size()*2);
        image.data_buffer_count=2;
        image.compiler_version_raw=static_cast<uint32_t>(4*uniforms.size());
        image.buffer_flags=0x10000000;
        image.default_uniform_buffer_count=4*uniforms.size();
    } else {
        uint32_t uniform_words=0;
        for (const auto &uniform:uniforms)
            uniform_words=std::max<uint32_t>(uniform_words,uniform.resource_index+uniform.components);
        uniform_words=(uniform_words+1u)&~1u;
        image.sdk_version=0x0165;
        image.program_flags=0x00081001;
        image.buffer_flags=0x10000000;
        image.primary_register_count=static_cast<uint16_t>(float_input_count*4u);
        image.secondary_register_count=static_cast<uint16_t>(uniform_words+literal_values.size());
        image.fragment_additional_inputs=static_cast<uint8_t>(float_input_count-1);
        image.fragment_input_components=4;
        image.default_uniform_buffer_count=uniform_words;
        image.data_buffer_count=static_cast<uint32_t>(literal_values.size());
        image.compiler_version_raw=0x0002df30;
    }
    image.primary_instructions=compiled.words.data();
    image.primary_instruction_count=compiled.words.size();
    image.containers=containers.data();
    image.container_count=containers.size();
    image.parameters=parameters.data();
    image.parameter_count=parameters.size();
    image.literals=literals.data();
    image.literal_count=literals.size();

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
