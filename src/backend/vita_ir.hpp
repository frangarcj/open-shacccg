#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace vsc::backend {

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

enum class IrOpKind : uint8_t {
    TransformPosition,
    ConstructPosition,
    CopyVarying,
};

struct IrOp {
    IrOpKind kind = IrOpKind::CopyVarying;
    uint32_t attribute = 0;
    uint32_t uniform = 0;
    IrVaryingSemantic semantic = IrVaryingSemantic::TexCoord;
};

struct VertexIr {
    std::vector<IrAttribute> attributes;
    std::vector<IrMatrix4Uniform> matrices;
    std::vector<IrOp> ops;
    uint32_t binary_guid = 0;
    uint32_t source_guid = 0;
};

struct IrCompileResult {
    std::vector<uint8_t> gxp;
    std::string error;
};

struct IrUniformVec4 {
    std::string name;
    uint32_t resource_index = 0;
};

enum class FragmentOpKind : uint8_t {
    UniformColor,
    VaryingColor,
    Texture2D,
    TextureTint2D,
    Arithmetic,
};

enum class FragmentExprKind : uint8_t {
    Uniform,
    Varying,
    Mul,
    Add,
    Sub,
    Min,
    Max,
    Neg,
    Abs,
    Dot,
    Splat,
};

struct FragmentExprNode {
    FragmentExprKind kind = FragmentExprKind::Varying;
    uint32_t a = 0; // child index for ops, uniform/varying index for leaves
    uint32_t b = 0; // second child for binary ops
    uint8_t components = 4;
};

struct IrSampler2D {
    std::string name;
    uint32_t resource_index = 0;
};

struct FragmentIr {
    FragmentOpKind op = FragmentOpKind::UniformColor;
    std::vector<IrUniformVec4> uniforms;
    std::vector<IrSampler2D> samplers;
    std::vector<FragmentExprNode> expressions;
    uint32_t root_expression = 0;
    uint32_t binary_guid = 0;
    uint32_t source_guid = 0;
};

bool compile_vertex_ir(const VertexIr &ir, IrCompileResult &out);
bool compile_fragment_ir(const FragmentIr &ir, IrCompileResult &out);

} // namespace vsc::backend
