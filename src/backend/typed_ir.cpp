#include "backend/typed_ir.hpp"

#include <array>

namespace vsc::backend {
namespace {

constexpr uint32_t kPayloadMask = 0x00ffffffu;
constexpr uint32_t kInvertBit = 1u << 24;
constexpr unsigned kTypeShift = 25;
constexpr unsigned kKindShift = 29;

TypedValue pack_value(TypedValueKind kind, TypedType type, uint32_t payload, bool inverted) {
    TypedValue out{};
    if (payload > kPayloadMask) return out;
    out.bits = (static_cast<uint32_t>(kind) << kKindShift) |
        (static_cast<uint32_t>(type) << kTypeShift) |
        (inverted ? kInvertBit : 0u) | payload;
    return out;
}

enum class TypedRole : uint8_t { None, ValueUse, ValueDef, PredicateUse, PredicateDef };
struct TypedOpcodeDesc { const char *name; std::array<TypedRole, 3> roles; };

constexpr TypedOpcodeDesc kTypedOpcodeDesc[] = {
#define VSC_TYPED_OP(name, label, dst, src0, src1) \
    {label, {TypedRole::dst, TypedRole::src0, TypedRole::src1}},
#include "backend/typed_ir_ops.inc"
#undef VSC_TYPED_OP
};
static_assert(std::size(kTypedOpcodeDesc) == static_cast<size_t>(TypedOpcode::Count));

const TypedOpcodeDesc *descriptor(TypedOpcode opcode) {
    const auto index = static_cast<size_t>(opcode);
    return index < std::size(kTypedOpcodeDesc) ? &kTypedOpcodeDesc[index] : nullptr;
}

MachineType machine_type(TypedType type) {
    switch (type) {
    case TypedType::F32: return MachineType::F32;
    case TypedType::F16: return MachineType::F16;
    case TypedType::U32: return MachineType::U32;
    case TypedType::U16: return MachineType::U16;
    case TypedType::S32: return MachineType::S32;
    case TypedType::Invalid: return MachineType::Invalid;
    }
    return MachineType::Invalid;
}

bool is_value(const TypedValue &value) {
    return value.kind() == TypedValueKind::Value || value.kind() == TypedValueKind::Literal;
}

bool valid_value_use(const TypedProgram &program, const TypedValue &value,
                     const std::vector<TypedType> &types, const std::vector<bool> &defined) {
    if (!is_value(value) || value.type() == TypedType::Invalid) return false;
    if (value.kind() == TypedValueKind::Literal)
        return value.id() < program.literals().size() && value.type() == TypedType::U32;
    return value.id() < defined.size() && defined[value.id()] && types[value.id()] == value.type();
}

MachineOperand lower_value(const TypedProgram &typed, const TypedValue &value,
                           const std::vector<MachineOperand> &values,
                           std::vector<MachineOperand> &literals,
                           MachineProgram &machine) {
    if (value.kind() == TypedValueKind::Value)
        return value.id() < values.size() ? values[value.id()] : MachineOperand{};
    if (value.kind() != TypedValueKind::Literal || value.id() >= typed.literals().size()) return {};
    if (literals[value.id()].kind() == MachineOperandKind::None)
        literals[value.id()] = machine.literal_u32(typed.literals()[value.id()]);
    return literals[value.id()];
}

} // namespace

TypedValue TypedValue::value(uint32_t id, TypedType type) {
    return pack_value(TypedValueKind::Value, type, id, false);
}
TypedValue TypedValue::predicate(uint32_t id, bool inverted) {
    return pack_value(TypedValueKind::Predicate, TypedType::Invalid, id, inverted);
}
TypedValue TypedValue::literal(uint32_t id, TypedType type) {
    return pack_value(TypedValueKind::Literal, type, id, false);
}
TypedValueKind TypedValue::kind() const {
    return static_cast<TypedValueKind>((bits >> kKindShift) & 0x7u);
}
TypedType TypedValue::type() const {
    return static_cast<TypedType>((bits >> kTypeShift) & 0xfu);
}
uint32_t TypedValue::id() const { return bits & kPayloadMask; }
bool TypedValue::inverted() const { return (bits & kInvertBit) != 0; }

TypedOpcode TypedInstruction::opcode() const { return static_cast<TypedOpcode>(code & 0xffu); }
uint8_t TypedInstruction::subop() const { return static_cast<uint8_t>(code >> 8); }

TypedValue TypedProgram::make_value(TypedType type) {
    if (type == TypedType::Invalid || next_value_ > kPayloadMask) return {};
    return TypedValue::value(next_value_++, type);
}
TypedValue TypedProgram::make_predicate(bool inverted) {
    if (next_predicate_ >= 1024) return {};
    return TypedValue::predicate(next_predicate_++, inverted);
}
TypedValue TypedProgram::literal_u32(uint32_t value) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(value);
    return TypedValue::literal(static_cast<uint32_t>(literals_.size() - 1), TypedType::U32);
}

bool TypedProgram::append(TypedOpcode opcode, uint8_t subop, TypedValue dst,
                          TypedValue src0, TypedValue src1, uint16_t aux) {
    if (!descriptor(opcode)) return false;
    TypedInstruction instruction{};
    instruction.code = static_cast<uint16_t>(static_cast<uint8_t>(opcode)) |
        (static_cast<uint16_t>(subop) << 8);
    instruction.aux = aux;
    instruction.dst = dst;
    instruction.src0 = src0;
    instruction.src1 = src1;
    instructions_.push_back(instruction);
    return true;
}

bool lower_typed_program(const TypedProgram &typed, MachineProgram &machine, std::string &error) {
    machine = {};
    error.clear();
    std::vector<MachineOperand> values(typed.value_count());
    std::vector<MachineOperand> predicates(typed.predicate_count());
    std::vector<MachineOperand> literals(typed.literals().size());
    std::vector<TypedType> value_types(typed.value_count(), TypedType::Invalid);
    std::vector<bool> value_defined(typed.value_count(), false);
    std::vector<bool> predicate_defined(typed.predicate_count(), false);

    for (const auto &instruction : typed.instructions()) {
        const auto *desc = descriptor(instruction.opcode());
        if (!desc) { error = "unknown typed opcode"; return false; }
        const TypedValue operands[] = {instruction.dst, instruction.src0, instruction.src1};
        for (size_t slot = 0; slot < 3; ++slot) {
            const auto role = desc->roles[slot];
            const auto &operand = operands[slot];
            if (role == TypedRole::None) {
                if (operand.kind() != TypedValueKind::None) { error = std::string(desc->name) + " has unexpected operand"; return false; }
            } else if (role == TypedRole::ValueDef) {
                if (operand.kind() != TypedValueKind::Value || operand.type() == TypedType::Invalid ||
                    operand.id() >= value_defined.size() || value_defined[operand.id()]) {
                    error = std::string(desc->name) + " has invalid value destination"; return false;
                }
            } else if (role == TypedRole::ValueUse) {
                if (!valid_value_use(typed, operand, value_types, value_defined)) {
                    error = std::string(desc->name) + " has invalid value source"; return false;
                }
            } else if (role == TypedRole::PredicateDef) {
                if (operand.kind() != TypedValueKind::Predicate || operand.inverted() ||
                    operand.id() >= predicate_defined.size() || predicate_defined[operand.id()]) {
                    error = std::string(desc->name) + " has invalid predicate destination"; return false;
                }
            } else if (role == TypedRole::PredicateUse) {
                if (operand.kind() != TypedValueKind::Predicate || operand.id() >= predicate_defined.size() ||
                    !predicate_defined[operand.id()]) {
                    error = std::string(desc->name) + " has invalid predicate source"; return false;
                }
            }
        }

        switch (instruction.opcode()) {
        case TypedOpcode::Input:
        case TypedOpcode::Uniform: {
            if (instruction.aux >= 128) { error = "resource index exceeds current register subset"; return false; }
            const MachineType type = machine_type(instruction.dst.type());
            if (type == MachineType::Invalid) { error = "unsupported typed resource type"; return false; }
            const auto bank = instruction.opcode() == TypedOpcode::Input ?
                usse::RegisterBank::PrimaryAttribute : usse::RegisterBank::SecondaryAttribute;
            values[instruction.dst.id()] = machine.physical(bank, static_cast<uint8_t>(instruction.aux), type);
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::Bitwise: {
            if (instruction.dst.type() != TypedType::U32 || instruction.src0.type() != TypedType::U32 ||
                instruction.src1.type() != TypedType::U32 ||
                instruction.subop() > static_cast<uint8_t>(usse::BitwiseOp::ArithmeticShiftRight)) {
                error = "typed bitwise currently requires U32 operands"; return false;
            }
            const auto dst = machine.make_value<MachineType::U32>();
            const auto src0 = lower_value(typed, instruction.src0, values, literals, machine);
            const auto src1 = lower_value(typed, instruction.src1, values, literals, machine);
            if (dst.kind() == MachineOperandKind::None || src0.kind() == MachineOperandKind::None || src1.kind() == MachineOperandKind::None ||
                !machine.emit<MachineOpcode::Bitwise>(instruction.subop(), dst, src0, src1)) {
                error = "failed to lower typed bitwise operation"; return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = TypedType::U32;
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::Compare: {
            if (instruction.src0.type() != TypedType::U32 || instruction.src1.type() != TypedType::U32 ||
                instruction.subop() > static_cast<uint8_t>(usse::CompareOp::GreaterEqual)) {
                error = "typed compare currently requires U32 operands"; return false;
            }
            const auto dst = machine.make_predicate();
            const auto src0 = lower_value(typed, instruction.src0, values, literals, machine);
            const auto src1 = lower_value(typed, instruction.src1, values, literals, machine);
            if (src0.kind() == MachineOperandKind::Literal || src1.kind() == MachineOperandKind::Literal) {
                error = "typed compare literals are not in the validated VTST subset"; return false;
            }
            if (dst.kind() == MachineOperandKind::None || src0.kind() == MachineOperandKind::None || src1.kind() == MachineOperandKind::None ||
                !machine.emit<MachineOpcode::Compare>(instruction.subop(), dst, src0, src1)) {
                error = "failed to lower typed compare"; return false;
            }
            predicates[instruction.dst.id()] = dst;
            predicate_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::Discard: {
            auto predicate = predicates[instruction.src0.id()];
            if (instruction.src0.inverted()) predicate = MachineOperand::virtual_predicate(predicate.id(), true);
            if (!machine.emit<MachineOpcode::Kill>(0, {}, predicate)) {
                error = "failed to lower typed discard"; return false;
            }
            break;
        }
        case TypedOpcode::Count:
            error = "invalid typed opcode sentinel";
            return false;
        }
    }
    return true;
}

bool compile_typed_program(const TypedProgram &typed, MachineCompileResult &out) {
    MachineProgram machine;
    std::string error;
    if (!lower_typed_program(typed, machine, error)) {
        out = {};
        out.error = error;
        return false;
    }
    return compile_machine_program(machine, out);
}

} // namespace vsc::backend
