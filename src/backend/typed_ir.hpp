#pragma once

#include "backend/machine_ir.hpp"

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
};

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

class TypedProgram {
public:
    TypedValue make_value(TypedType type);
    template <TypedType Type>
    TypedValue make_value() { return make_value(Type); }

    TypedValue make_predicate(bool inverted = false);
    TypedValue literal_u32(uint32_t value);

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
        auto dst = make_value<Type>();
        if (!emit<TypedOpcode::Input>(0, dst, {}, {}, location)) return {};
        return dst;
    }

    template <TypedType Type>
    TypedValue uniform(uint16_t resource_index) {
        auto dst = make_value<Type>();
        if (!emit<TypedOpcode::Uniform>(0, dst, {}, {}, resource_index)) return {};
        return dst;
    }

    const std::vector<TypedInstruction> &instructions() const { return instructions_; }
    const std::vector<uint32_t> &literals() const { return literals_; }
    uint32_t value_count() const { return next_value_; }
    uint16_t predicate_count() const { return next_predicate_; }

private:
    std::vector<TypedInstruction> instructions_;
    std::vector<uint32_t> literals_;
    uint32_t next_value_ = 0;
    uint16_t next_predicate_ = 0;
};

bool lower_typed_program(const TypedProgram &typed, MachineProgram &machine, std::string &error);
bool compile_typed_program(const TypedProgram &typed, MachineCompileResult &out);

} // namespace vsc::backend
