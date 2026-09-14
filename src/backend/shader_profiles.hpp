#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vsc::backend {

class MachineProgram;

struct IrAttribute {
    std::string name;
    uint8_t components = 4; // logical source width (GXP reflection remains vec4)
    uint32_t resource_index = 0;
};

struct IrMatrix4Uniform {
    std::string name;
    uint32_t resource_index = 0;
};

enum class IrVaryingSemantic : uint8_t { TexCoord, Color };

struct IrCompileResult {
    std::vector<uint8_t> gxp;
    std::string error;
};

struct IrUniformVec4 {
    std::string name;
    uint32_t resource_index = 0;
};

struct IrUniformS32 {
    std::string name;
    uint32_t resource_index = 0;
};

struct IrSampler2D {
    std::string name;
    uint32_t resource_index = 0;
};

enum class FragmentMachineProfile : uint8_t {
    UniformColor,
    VaryingColor,
    Texture2D,
    TextureTint2D,
    SwizzleWzyx,
    ConstantRed,
};

bool compile_vertex_construct_position(const IrAttribute &position,
                                       uint32_t binary_guid, uint32_t source_guid,
                                       IrCompileResult &out);
bool compile_vertex_matrix_path(const IrAttribute &position, const IrAttribute &varying,
                                const IrMatrix4Uniform &matrix, IrVaryingSemantic semantic,
                                uint32_t binary_guid, uint32_t source_guid,
                                IrCompileResult &out);
bool compile_vertex_passthrough(const IrAttribute &position,
                                uint32_t binary_guid, uint32_t source_guid,
                                IrCompileResult &out);
bool compile_vertex_passthrough_varying(const IrAttribute &position, const IrAttribute &varying,
                                        IrVaryingSemantic semantic,
                                        uint32_t binary_guid, uint32_t source_guid,
                                        IrCompileResult &out);
bool compile_vertex_uniform_matrix(const IrAttribute &position, const IrMatrix4Uniform &matrix,
                                   uint32_t binary_guid, uint32_t source_guid,
                                   IrCompileResult &out);
bool compile_fragment_machine_profile(FragmentMachineProfile profile,
                                      const std::vector<IrUniformVec4> &uniforms,
                                      const std::vector<IrSampler2D> &samplers,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out);
bool compile_fragment_arithmetic_machine(const MachineProgram &primary,
                                         const std::vector<IrUniformVec4> &uniforms,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out);
bool compile_fragment_control_machine(const MachineProgram &primary,
                                      uint8_t float4_input_count,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out);
bool compile_fragment_loop_machine(const MachineProgram &primary,
                                   const IrUniformS32 &uniform,
                                   uint32_t binary_guid, uint32_t source_guid,
                                   IrCompileResult &out);

} // namespace vsc::backend
