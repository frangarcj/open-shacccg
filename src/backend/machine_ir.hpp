#pragma once

#include "backend/program_builder.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace vsc::backend {

enum class MachineType : uint8_t {
    Invalid = 0,
    F32,
    F16,
    U32,
    U16,
    S32,
};

enum class MachineOperandKind : uint8_t {
    None = 0,
    VirtualValue,
    PhysicalValue,
    VirtualPredicate,
    PhysicalPredicate,
    Literal,
};

// Compact operand handle. Values, predicates and literals share one encoding so
// instruction descriptors can drive validation/allocation without per-op structs.
struct MachineOperand {
    uint32_t bits = 0;

    static MachineOperand virtual_value(uint32_t id, MachineType type);
    static MachineOperand physical_value(usse::RegisterBank bank, uint8_t num, MachineType type);
    static MachineOperand virtual_predicate(uint32_t id, bool inverted = false);
    static MachineOperand physical_predicate(uint8_t num, bool inverted = false);
    static MachineOperand literal(uint32_t id, MachineType type);

    MachineOperandKind kind() const;
    MachineType type() const;
    uint32_t id() const;
    bool inverted() const;
    usse::RegisterRef physical_register() const;
};
static_assert(sizeof(MachineOperand) == 4, "machine operands must stay compact");

enum class MachineOpcode : uint8_t {
#define VSC_MACHINE_OP(name, label, dst, src0, src1, predicates) name,
#include "backend/machine_ops.inc"
#undef VSC_MACHINE_OP
    Count,
};

// Generic 16-byte instruction. `subop` is opcode-specific (for example
// usse::CompareOp); `control` carries an optional virtual guard predicate.
struct MachineInstruction {
    uint16_t code = 0;
    uint16_t control = 0;
    MachineOperand dst{};
    MachineOperand src0{};
    MachineOperand src1{};

    MachineOpcode opcode() const;
    uint8_t subop() const;
    bool has_guard() const;
    bool guard_inverted() const;
    bool guard_is_physical() const;
    uint16_t guard_id() const;
};
static_assert(sizeof(MachineInstruction) == 16, "machine instructions must stay compact");

class MachineProgram {
public:
    MachineOperand make_value(MachineType type);
    template <MachineType Type>
    MachineOperand make_value() { return make_value(Type); }

    MachineOperand make_predicate(bool inverted = false);
    MachineOperand physical(usse::RegisterBank bank, uint8_t num, MachineType type) const;
    template <MachineType Type>
    MachineOperand physical(usse::RegisterBank bank, uint8_t num) const {
        return physical(bank, num, Type);
    }

    MachineOperand literal_u32(uint32_t value);

    bool append(MachineOpcode opcode, uint8_t subop = 0,
                MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {},
                MachineOperand guard = {});

    template <MachineOpcode Opcode>
    bool emit(uint8_t subop = 0,
              MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {},
              MachineOperand guard = {}) {
        static_assert(Opcode < MachineOpcode::Count, "invalid machine opcode");
        return append(Opcode, subop, dst, src0, src1, guard);
    }

    const std::vector<MachineInstruction> &instructions() const { return instructions_; }
    const std::vector<uint32_t> &literals() const { return literals_; }
    uint32_t value_count() const { return next_value_; }
    uint16_t predicate_count() const { return next_predicate_; }

private:
    std::vector<MachineInstruction> instructions_;
    std::vector<uint32_t> literals_;
    uint32_t next_value_ = 0;
    uint16_t next_predicate_ = 0;
};

struct MachineCompileResult {
    std::vector<uint64_t> words;
    std::vector<uint8_t> predicate_registers;
    std::string error;
};

bool compile_machine_program(const MachineProgram &program, MachineCompileResult &out);

} // namespace vsc::backend
