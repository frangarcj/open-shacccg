#pragma once

#include "backend/machine_ir.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace vsc::backend {

enum class TypedType : uint8_t {
    Invalid = 0,
    F32,
    F16,
    U32,
    U16,
    S32,
    F32x2,
    F32x3,
    F32x4,
    F16x2,
    F16x3,
    F16x4,
    U32x2,
    U32x3,
    U32x4,
    Sampler2D,
};

static_assert(static_cast<uint8_t>(TypedType::Sampler2D) < 16,
              "TypedType must remain encodable in the compact 4-bit handle field");

enum class TypedValueKind : uint8_t {
    None = 0,
    Value,
    Predicate,
    Literal,
};

struct TypedValue {
    uint32_t bits = 0;

    static TypedValue value(uint32_t id, TypedType type);
    static TypedValue predicate(uint32_t id, bool inverted = false);
    static TypedValue literal(uint32_t id, TypedType type);

    TypedValueKind kind() const;
    TypedType type() const;
    uint32_t id() const;
    bool inverted() const;
};
static_assert(sizeof(TypedValue) == 4, "typed IR handles must stay compact");

enum class TypedOpcode : uint8_t {
#define VSC_TYPED_OP(name, label, dst, src0, src1) name,
#include "backend/typed_ir_ops.inc"
#undef VSC_TYPED_OP
    Count,
};

struct TypedInstruction {
    uint16_t code = 0;
    uint16_t aux = 0;
    TypedValue dst{};
    TypedValue src0{};
    TypedValue src1{};

    TypedOpcode opcode() const;
    uint8_t subop() const;
};
static_assert(sizeof(TypedInstruction) == 16, "typed IR instructions must stay compact");

enum class TypedStage : uint8_t { Vertex, Fragment };

enum class TypedResourceKind : uint8_t {
    Input,
    Uniform,
    Matrix3,
    Matrix4,
    Sampler2D,
    Output,
};

enum class TypedSemantic : uint8_t {
    None,
    Position,
    Color,
    TexCoord,
    PointCoord,
    PointSize,
};

enum class TypedFloatOp : uint8_t {
    Mul,
    Add,
    Sub,
    Min,
    Max,
    Dot,
    Div,
};

enum class TypedFloatUnaryOp : uint8_t {
    Neg,
    Abs,
    Saturate,
    Rsqrt,
    Log2,
    Exp2,
    Floor,
};

enum class TypedFloatSwizzleOp : uint8_t {
    Wzyx,
    XY,
    XYZ,
};

enum class TypedFloatConvertOp : uint8_t {
    F32x4ToF16x4,
};

// Resource metadata is deliberately separate from the hot 16-byte instruction
// stream. The value handle links data-bearing resources back to Typed IR while
// name_index keeps strings out of every descriptor.
struct TypedResource {
    TypedValue value{};
    uint16_t name_index = 0;
    uint16_t index = 0;
    TypedResourceKind kind = TypedResourceKind::Input;
    TypedType type = TypedType::Invalid;
    TypedSemantic semantic = TypedSemantic::None;
    uint8_t semantic_index = 0;
};
static_assert(sizeof(TypedResource) == 12, "typed resource descriptors must stay compact");

struct TypedFloatSelectDesc {
    TypedValue predicate{};
    TypedValue true_value{};
    TypedValue false_value{};
};
static_assert(sizeof(TypedFloatSelectDesc)==12,"typed select side-table entries must stay compact");

class TypedProgram {
public:
    TypedValue make_value(TypedType type);
    template <TypedType Type>
    TypedValue make_value() { return make_value(Type); }

    TypedValue make_predicate(bool inverted = false);
    TypedValue literal_u32(uint32_t value);
    TypedValue literal_s32(int32_t value);
    TypedValue literal_f32(uint32_t bits);
    TypedValue literal_f32x4(const std::array<uint32_t,4> &bits);
    TypedValue compose_f32x3(const std::array<TypedValue,3> &components);
    TypedValue compose_f32x4(const std::array<TypedValue,4> &components);
    TypedValue select_f32(TypedValue predicate, TypedValue true_value, TypedValue false_value);
    TypedValue sampler(uint16_t binding);

    TypedValue input(TypedType type, uint16_t location);
    TypedValue input_component_f32(uint16_t physical_index, uint8_t component);
    TypedValue uniform(TypedType type, uint16_t resource_index);
    uint16_t make_label();
    bool bind_label(uint16_t label);
    bool jump(uint16_t label);
    bool branch(uint16_t label, TypedValue predicate);

    bool append(TypedOpcode opcode, uint8_t subop = 0,
                TypedValue dst = {}, TypedValue src0 = {}, TypedValue src1 = {},
                uint16_t aux = 0);

    template <TypedOpcode Opcode>
    bool emit(uint8_t subop = 0,
              TypedValue dst = {}, TypedValue src0 = {}, TypedValue src1 = {},
              uint16_t aux = 0) {
        static_assert(Opcode < TypedOpcode::Count, "invalid typed opcode");
        return append(Opcode, subop, dst, src0, src1, aux);
    }

    template <TypedType Type>
    TypedValue input(uint16_t location) {
        return input(Type, location);
    }

    template <TypedType Type>
    TypedValue uniform(uint16_t resource_index) {
        return uniform(Type, resource_index);
    }

    const std::vector<TypedInstruction> &instructions() const { return instructions_; }
    const std::vector<uint32_t> &literals() const { return literals_; }
    const std::vector<std::array<uint32_t,4>> &float4_literals() const { return float4_literals_; }
    const std::vector<std::array<TypedValue,3>> &float3_composites() const { return float3_composites_; }
    const std::vector<std::array<TypedValue,4>> &float4_composites() const { return float4_composites_; }
    const std::vector<TypedFloatSelectDesc> &float_selects() const { return float_selects_; }
    const std::vector<uint32_t> &labels() const { return labels_; }
    uint32_t value_count() const { return next_value_; }
    uint16_t predicate_count() const { return next_predicate_; }

private:
    std::vector<TypedInstruction> instructions_;
    std::vector<uint32_t> literals_;
    std::vector<std::array<uint32_t,4>> float4_literals_;
    std::vector<std::array<TypedValue,3>> float3_composites_;
    std::vector<std::array<TypedValue,4>> float4_composites_;
    std::vector<TypedFloatSelectDesc> float_selects_;
    std::vector<uint32_t> labels_;
    uint32_t next_value_ = 0;
    uint16_t next_predicate_ = 0;
};

class TypedShader {
public:
    explicit TypedShader(TypedStage stage = TypedStage::Fragment) : stage_(stage) {}

    TypedStage stage() const { return stage_; }
    TypedProgram &program() { return program_; }
    const TypedProgram &program() const { return program_; }

    uint16_t add_resource(TypedResourceKind kind, TypedValue value, TypedType type,
                          const std::string &name, uint16_t index,
                          TypedSemantic semantic = TypedSemantic::None,
                          uint8_t semantic_index = 0);

    const std::vector<TypedResource> &resources() const { return resources_; }
    const std::string &resource_name(const TypedResource &resource) const;

private:
    TypedStage stage_ = TypedStage::Fragment;
    TypedProgram program_;
    std::vector<TypedResource> resources_;
    std::vector<std::string> names_;
};

struct IrCompileResult;

bool lower_typed_program(const TypedProgram &typed, MachineProgram &machine, std::string &error);
bool compile_typed_program(const TypedProgram &typed, MachineCompileResult &out);
bool compile_typed_shader(const TypedShader &shader, IrCompileResult &out);

uint8_t typed_component_count(TypedType type);
bool typed_is_float(TypedType type);

} // namespace vsc::backend
