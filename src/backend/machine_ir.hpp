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

enum class MachineRegisterClass : uint8_t {
    ScalarTemp,
    FloatTemp,
    Gpi,
    VmadAccumulator,
};

enum class MachineRegisterOrder : uint8_t { Low, High };

// Compact live-range request shared by Machine IR and the legacy Vita lowering
// while float operations migrate into the 16-byte instruction stream.
struct MachineLiveRange {
    uint16_t start = 0;
    uint16_t end = 0; // exclusive
    MachineType type = MachineType::Invalid;
    MachineRegisterClass reg_class = MachineRegisterClass::ScalarTemp;
    MachineRegisterOrder order = MachineRegisterOrder::Low;
    uint8_t width = 1;
};
static_assert(sizeof(MachineLiveRange) == 8, "machine live ranges must stay compact");

struct MachineValueDesc {
    MachineType type = MachineType::Invalid;
    MachineRegisterClass reg_class = MachineRegisterClass::ScalarTemp;
    MachineRegisterOrder order = MachineRegisterOrder::Low;
    uint8_t width = 1;
};
static_assert(sizeof(MachineValueDesc) == 4, "machine value descriptors must stay compact");

bool allocate_machine_live_ranges(const std::vector<MachineLiveRange> &ranges,
                                  std::vector<usse::RegisterRef> &assignment,
                                  std::string &error);

constexpr usse::RegisterRef machine_primary(uint8_t num) {
    return {usse::RegisterBank::PrimaryAttribute, num};
}
constexpr usse::RegisterRef machine_secondary(uint8_t num) {
    return {usse::RegisterBank::SecondaryAttribute, num};
}
constexpr usse::RegisterRef machine_vertex_output(uint8_t num) {
    return {usse::RegisterBank::Output, num};
}
constexpr usse::RegisterRef machine_fragment_output(uint8_t num) {
    return {usse::RegisterBank::PrimaryAttribute, num};
}
constexpr usse::RegisterRef machine_special(uint8_t num) {
    return {usse::RegisterBank::Special, num};
}
constexpr usse::RegisterRef machine_immediate(uint8_t num) {
    return {usse::RegisterBank::Immediate, num};
}
constexpr uint8_t machine_gpi_index(usse::RegisterRef reg) {
    return reg.bank == usse::RegisterBank::Temp && reg.num >= 124 && reg.num < 128
        ? static_cast<uint8_t>(reg.num - 124) : 0xff;
}

enum class MachineOperandKind : uint8_t {
    None = 0,
    VirtualValue,
    PhysicalValue,
    VirtualPredicate,
    PhysicalPredicate,
    Literal,
    VirtualPair,
    Label,
};

// Compact operand handle. Values, predicates and literals share one encoding so
// instruction descriptors can drive validation/allocation without per-op structs.
struct MachineOperand {
    uint32_t bits = 0;

    static MachineOperand virtual_value(uint32_t id, MachineType type);
    static MachineOperand physical_value(usse::RegisterBank bank, uint8_t num, MachineType type,
                                         uint8_t component = 0xff);
    static MachineOperand virtual_predicate(uint32_t id, bool inverted = false);
    static MachineOperand physical_predicate(uint8_t num, bool inverted = false);
    static MachineOperand literal(uint32_t id, MachineType type);
    static MachineOperand virtual_pair(uint16_t first, uint16_t second, MachineType type);
    static MachineOperand label(uint32_t id);

    MachineOperandKind kind() const;
    MachineType type() const;
    uint32_t id() const;
    bool inverted() const;
    usse::RegisterRef physical_register() const;
    uint8_t physical_component() const;
    uint16_t pair_first() const;
    uint16_t pair_second() const;
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
    uint16_t config() const;
};
static_assert(sizeof(MachineInstruction) == 16, "machine instructions must stay compact");

enum class MachineVectorSwizzle : uint8_t {
    Identity,
    PositionXY11,
    Source2YYYY,
    PositionZW01,
};

constexpr uint8_t machine_pack_subop(usse::PackFormat src, usse::PackFormat dst) {
    return static_cast<uint8_t>(src) | (static_cast<uint8_t>(dst) << 3);
}

constexpr uint16_t machine_move_config(uint8_t mask, uint8_t swizzle = 4,
                                       uint8_t repeat = 0, bool skip_invalid = true,
                                       bool no_schedule = false, bool end = false) {
    return (mask & 0x0f) | ((swizzle & 0x0f) << 4) | ((repeat & 0x03) << 8) |
        (skip_invalid ? 0x0400 : 0) | (no_schedule ? 0x0800 : 0) | (end ? 0x1000 : 0);
}

constexpr uint16_t machine_pack_config(uint8_t mask, bool skip_invalid = true,
                                       bool no_schedule = true, bool end = false) {
    return (mask & 0x0f) | (skip_invalid ? 0x0010 : 0) |
        (no_schedule ? 0x0020 : 0) | (end ? 0x0040 : 0);
}

constexpr uint16_t machine_vector_config(uint8_t mask,
                                         MachineVectorSwizzle swizzle = MachineVectorSwizzle::Identity,
                                         bool src0_negative = false,
                                         bool src0_absolute = false,
                                         bool src1_absolute = false,
                                         bool skip_invalid = true,
                                         bool no_schedule = false) {
    return (mask & 0x0f) | ((static_cast<uint16_t>(swizzle) & 0x03) << 4) |
        (src0_negative ? 0x0040 : 0) | (src0_absolute ? 0x0080 : 0) |
        (src1_absolute ? 0x0100 : 0) | (skip_invalid ? 0x0200 : 0) |
        (no_schedule ? 0x0400 : 0);
}

constexpr uint16_t machine_complex_config(bool no_schedule = false, bool end = false) {
    return (no_schedule ? 0x0001u : 0u) | (end ? 0x0002u : 0u);
}

constexpr uint16_t machine_nop_config(bool scheduling_allowed = false, bool end = false) {
    return (scheduling_allowed ? 0x0001u : 0u) | (end ? 0x0002u : 0u);
}

constexpr uint16_t machine_vmad_config(uint8_t write_mask, bool no_schedule = false) {
    return (write_mask & 0x0f) | (no_schedule ? 0x0010 : 0);
}

constexpr uint16_t machine_vmad_uniform_mat4_config(bool no_schedule = false) {
    return no_schedule ? 0x0001u : 0u;
}

constexpr uint16_t machine_texcoord_mat4_xy_config(bool no_schedule_last = false) {
    return no_schedule_last ? 0x0001u : 0u;
}

class MachineProgram {
public:
    MachineOperand make_value(MachineType type);
    MachineOperand make_value(MachineType type, MachineRegisterClass reg_class,
                              uint8_t width = 1,
                              MachineRegisterOrder order = MachineRegisterOrder::Low);
    template <MachineType Type>
    MachineOperand make_value() { return make_value(Type); }
    template <MachineType Type>
    MachineOperand make_value(MachineRegisterClass reg_class, uint8_t width = 1,
                              MachineRegisterOrder order = MachineRegisterOrder::Low) {
        return make_value(Type, reg_class, width, order);
    }

    MachineOperand make_predicate(bool inverted = false);
    MachineOperand physical(usse::RegisterBank bank, uint8_t num, MachineType type,
                            uint8_t component = 0xff) const;
    MachineOperand physical(usse::RegisterRef reg, MachineType type, uint8_t component = 0xff) const;
    template <MachineType Type>
    MachineOperand physical(usse::RegisterBank bank, uint8_t num) const {
        return physical(bank, num, Type);
    }

    MachineOperand literal_u32(uint32_t value);
    MachineOperand literal_s32(int32_t value);
    MachineOperand pair(MachineOperand first, MachineOperand second) const;
    MachineOperand make_label();
    bool bind_label(MachineOperand label);
    bool branch(MachineOperand target, MachineOperand guard = {});

    bool append(MachineOpcode opcode, uint8_t subop = 0,
                MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {},
                MachineOperand guard = {});
    bool append_config(MachineOpcode opcode, uint8_t subop, uint16_t config,
                       MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {});

    template <MachineOpcode Opcode>
    bool emit(uint8_t subop = 0,
              MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {},
              MachineOperand guard = {}) {
        static_assert(Opcode < MachineOpcode::Count, "invalid machine opcode");
        return append(Opcode, subop, dst, src0, src1, guard);
    }

    template <MachineOpcode Opcode>
    bool emit_config(uint8_t subop, uint16_t config,
                     MachineOperand dst = {}, MachineOperand src0 = {}, MachineOperand src1 = {}) {
        static_assert(Opcode < MachineOpcode::Count, "invalid machine opcode");
        return append_config(Opcode, subop, config, dst, src0, src1);
    }

    const std::vector<MachineInstruction> &instructions() const { return instructions_; }
    const std::vector<uint32_t> &literals() const { return literals_; }
    const std::vector<uint32_t> &labels() const { return labels_; }
    const std::vector<MachineValueDesc> &value_descs() const { return value_descs_; }
    uint32_t value_count() const { return static_cast<uint32_t>(value_descs_.size()); }
    uint16_t predicate_count() const { return next_predicate_; }

private:
    std::vector<MachineInstruction> instructions_;
    std::vector<uint32_t> literals_;
    std::vector<uint32_t> labels_;
    std::vector<MachineValueDesc> value_descs_;
    uint16_t next_predicate_ = 0;
};

struct MachineCompileResult {
    std::vector<uint64_t> words;
    std::vector<uint8_t> predicate_registers;
    std::vector<usse::RegisterRef> value_registers;
    std::string error;
};

bool compile_machine_program(const MachineProgram &program, MachineCompileResult &out);

} // namespace vsc::backend
