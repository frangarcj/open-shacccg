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
    uint8_t semantic = 14; // GXP TEXCOORD by default
    uint8_t semantic_index = 0;
};

struct IrMatrix4Uniform {
    std::string name;
    uint32_t resource_index = 0;
};

struct IrMatrix3Uniform {
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

struct IrUniformFloat {
    std::string name;
    uint8_t components = 1;
    uint32_t resource_index = 0;
};

struct IrLiteralF32 {
    uint32_t resource_index = 0;
    uint32_t value_bits = 0;
};

struct IrSampler2D {
    std::string name;
    uint32_t resource_index = 0;
    uint8_t texcoord_index = 0;
    bool point_coord = false;
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
bool compile_vertex_construct_position_varying(const IrAttribute &position,
                                               const IrAttribute &varying,
                                               IrVaryingSemantic semantic,
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
bool compile_vertex_uniform_matrix_point_size(const IrAttribute &position,
                                              const IrMatrix4Uniform &matrix,
                                              const IrUniformFloat &point_size,
                                              uint32_t binary_guid, uint32_t source_guid,
                                              IrCompileResult &out);
bool compile_vertex_uniform_matrix_varying_point_size(const IrAttribute &position,
                                                      const IrAttribute &varying,
                                                      const IrMatrix4Uniform &matrix,
                                                      const IrUniformFloat &point_size,
                                                      IrVaryingSemantic semantic,
                                                      uint32_t binary_guid, uint32_t source_guid,
                                                      IrCompileResult &out);
bool compile_vertex_uniform_matrix_texcoord_point_size(const IrAttribute &position,
                                                      const IrAttribute &texcoord,
                                                      const IrMatrix4Uniform &position_matrix,
                                                      const IrMatrix4Uniform &texcoord_matrix,
                                                      const IrUniformFloat &point_size,
                                                      uint32_t binary_guid, uint32_t source_guid,
                                                      IrCompileResult &out);
bool compile_vertex_uniform_matrix_two_texcoords_color_point_size(
    const IrAttribute &position, const IrAttribute &texcoord0, const IrAttribute &texcoord1,
    const IrAttribute &color, const IrMatrix4Uniform &position_matrix,
    const IrMatrix4Uniform &texcoord_matrix0, const IrMatrix4Uniform &texcoord_matrix1,
    const IrUniformFloat &point_size, uint32_t binary_guid, uint32_t source_guid,
    IrCompileResult &out);
bool compile_vertex_uniform_matrix_three_texcoords_color_point_size(
    const IrAttribute &position, const IrAttribute &texcoord0, const IrAttribute &texcoord1,
    const IrAttribute &texcoord2, const IrAttribute &color, const IrMatrix4Uniform &position_matrix,
    const IrMatrix4Uniform &texcoord_matrix0, const IrMatrix4Uniform &texcoord_matrix1,
    const IrMatrix4Uniform &texcoord_matrix2, const IrUniformFloat &point_size,
    uint32_t binary_guid, uint32_t source_guid, IrCompileResult &out);
bool compile_vertex_multivarying_machine(
    const MachineProgram &primary,
    const std::vector<IrAttribute> &attributes,
    const std::vector<IrUniformFloat> &uniforms,
    const std::vector<IrMatrix4Uniform> &matrices,
    const IrMatrix3Uniform &normal_matrix,
    const std::vector<IrLiteralF32> &literals,
    uint32_t binary_guid, uint32_t source_guid, IrCompileResult &out);
bool compile_vertex_color_texcoord_point_machine(const MachineProgram &primary,
                                     const std::vector<IrAttribute> &attributes,
                                     const std::vector<IrUniformFloat> &uniforms,
                                     const std::vector<IrMatrix4Uniform> &matrices,
                                     const std::vector<IrMatrix3Uniform> &matrices3,
                                     const std::vector<IrLiteralF32> &literals,
                                     uint32_t binary_guid, uint32_t source_guid,
                                     IrCompileResult &out);
bool compile_vertex_clip_machine(const MachineProgram &primary,
                                 const std::vector<IrAttribute> &attributes,
                                 const std::vector<IrUniformFloat> &uniforms,
                                 const std::vector<IrMatrix4Uniform> &matrices,
                                 const std::vector<IrLiteralF32> &literals,
                                 uint32_t binary_guid, uint32_t source_guid,
                                 IrCompileResult &out);
bool compile_vertex_indexed_clear(const IrUniformVec4 &position,
                                  const IrUniformFloat &clear_depth,
                                  uint32_t binary_guid, uint32_t source_guid,
                                  IrCompileResult &out);
bool compile_vertex_generic_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                    const std::vector<IrAttribute> &attributes,
                                    const std::vector<IrUniformFloat> &uniforms,
                                    const std::vector<IrLiteralF32> &literals,
                                    IrVaryingSemantic varying_semantic,
                                    uint32_t binary_guid, uint32_t source_guid,
                                    IrCompileResult &out);
bool compile_fragment_machine_profile(FragmentMachineProfile profile,
                                      const std::vector<IrUniformVec4> &uniforms,
                                      const std::vector<IrSampler2D> &samplers,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out);
bool compile_fragment_linear_fog(const IrUniformVec4 &fog_color,
                                 const IrUniformFloat &fog_range,
                                 const IrUniformFloat &fog_far,
                                 uint32_t binary_guid, uint32_t source_guid,
                                 IrCompileResult &out);
bool compile_fragment_exp2_fog(const IrUniformVec4 &fog_color,
                               const IrUniformFloat &fog_density,
                               uint32_t binary_guid, uint32_t source_guid,
                               IrCompileResult &out);
bool compile_fragment_srgb(uint32_t binary_guid, uint32_t source_guid,
                           IrCompileResult &out);
bool compile_fragment_texture_alpha_select_machine(const IrUniformFloat &uniform,
                                                   const IrSampler2D &sampler,
                                                   uint32_t binary_guid, uint32_t source_guid,
                                                   IrCompileResult &out);
bool compile_fragment_texture_tint_alpha_discard(const IrUniformFloat &cut,
                                                 const IrUniformVec4 &tint,
                                                 const IrSampler2D &sampler,
                                                 uint32_t binary_guid, uint32_t source_guid,
                                                 IrCompileResult &out);
bool compile_fragment_two_texture_combine(const IrSampler2D &sampler0,
                                          const IrSampler2D &sampler1,
                                          uint32_t binary_guid, uint32_t source_guid,
                                          IrCompileResult &out);
bool compile_fragment_texture_control_machine(const MachineProgram &primary,
                                              const std::vector<IrUniformFloat> &uniforms,
                                              const std::vector<IrLiteralF32> &literals,
                                              const IrSampler2D &sampler,
                                              uint32_t binary_guid, uint32_t source_guid,
                                              IrCompileResult &out);
bool compile_fragment_s32_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                  const std::vector<IrUniformS32> &uniforms,
                                  uint32_t binary_guid, uint32_t source_guid,
                                  IrCompileResult &out);
bool compile_fragment_f32_to_s32_machine(const MachineProgram &primary,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out);
bool compile_fragment_s32_to_f32_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                         const IrUniformS32 &uniform,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out);
bool compile_fragment_s32x2_machine(const MachineProgram &primary, const MachineProgram &secondary,
                                    const std::vector<IrUniformS32> &uniforms,
                                    uint32_t binary_guid, uint32_t source_guid,
                                    IrCompileResult &out);
bool compile_fragment_arithmetic_machine(const MachineProgram &primary,
                                         const std::vector<IrUniformFloat> &uniforms,
                                         const std::vector<IrLiteralF32> &literals,
                                         uint8_t float_input_count,
                                         uint8_t float_components,
                                         uint32_t binary_guid, uint32_t source_guid,
                                         IrCompileResult &out);
bool compile_fragment_control_machine(const MachineProgram &primary,
                                      uint8_t float4_input_count,
                                      uint32_t binary_guid, uint32_t source_guid,
                                      IrCompileResult &out);
bool compile_fragment_cfg_machine(const MachineProgram &primary,
                                  const std::vector<IrAttribute> &inputs,
                                  const std::vector<IrUniformFloat> &uniforms,
                                  const std::vector<IrLiteralF32> &literals,
                                  uint32_t binary_guid, uint32_t source_guid,
                                  IrCompileResult &out);
bool compile_fragment_loop_machine(const MachineProgram &primary,
                                   const IrUniformS32 &uniform,
                                   uint32_t binary_guid, uint32_t source_guid,
                                   IrCompileResult &out);

} // namespace vsc::backend
