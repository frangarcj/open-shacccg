#include "backend/machine_ir.hpp"

#include <algorithm>
#include <array>
#include <limits>

namespace vsc::backend {
namespace {

constexpr uint32_t kPayloadMask = 0x00ffffffu;
constexpr uint32_t kInvertBit = 1u << 24;
constexpr unsigned kTypeShift = 25;
constexpr unsigned kKindShift = 29;

MachineOperand pack_operand(MachineOperandKind kind, MachineType type, uint32_t payload, bool inverted) {
    MachineOperand out{};
    if (payload > kPayloadMask) return out;
    out.bits = (static_cast<uint32_t>(kind) << kKindShift) |
        (static_cast<uint32_t>(type) << kTypeShift) |
        (inverted ? kInvertBit : 0u) | payload;
    return out;
}

enum class OperandRole : uint8_t {
    None,
    ValueUse,
    ValueDef,
    PredicateUse,
    PredicateDef,
};

struct OpcodeDesc {
    const char *name;
    std::array<OperandRole, 3> roles;
    uint8_t predicate_use_mask;
};

constexpr OpcodeDesc kOpcodeDesc[] = {
    {"compare", {OperandRole::PredicateDef, OperandRole::ValueUse, OperandRole::ValueUse}, 0x0f},
    {"kill",    {OperandRole::None,         OperandRole::PredicateUse, OperandRole::None}, 0x03},
};

const OpcodeDesc *descriptor(MachineOpcode opcode) {
    const auto index = static_cast<size_t>(opcode);
    return index < std::size(kOpcodeDesc) ? &kOpcodeDesc[index] : nullptr;
}

uint8_t allowed_predicates(const OpcodeDesc &desc, const MachineOperand &operand) {
    uint8_t mask = desc.predicate_use_mask;
    if (operand.inverted()) {
        // Extended predicates expose !p0/!p1; KILL's short predicate only !p0.
        mask &= desc.predicate_use_mask == 0x03 ? 0x01 : 0x03;
    }
    return mask;
}

uint8_t allowed_guard_predicates(const MachineInstruction &instruction) {
    if (!instruction.guard_inverted()) return 0x0f;
    return 0x03;
}

bool is_value_operand(const MachineOperand &operand) {
    return operand.kind() == MachineOperandKind::VirtualValue ||
        operand.kind() == MachineOperandKind::PhysicalValue ||
        operand.kind() == MachineOperandKind::Literal;
}

bool is_predicate_operand(const MachineOperand &operand) {
    return operand.kind() == MachineOperandKind::VirtualPredicate ||
        operand.kind() == MachineOperandKind::PhysicalPredicate;
}

bool predicate_from_physical(uint8_t reg, bool inverted, usse::Predicate *predicate) {
    if (!predicate || reg >= 4) return false;
    if (!inverted) {
        *predicate = static_cast<usse::Predicate>(static_cast<uint8_t>(usse::Predicate::P0) + reg);
        return true;
    }
    if (reg == 0) { *predicate = usse::Predicate::NotP0; return true; }
    if (reg == 1) { *predicate = usse::Predicate::NotP1; return true; }
    return false;
}

struct PredicateInterval {
    bool defined = false;
    uint32_t start = 0;
    uint32_t end = 0;
    uint8_t allowed = 0x0f;
};

bool resolve_predicate(const MachineInstruction &instruction, const MachineOperand &operand,
                       const std::vector<uint8_t> &assignment, usse::Predicate *predicate) {
    if (operand.kind() == MachineOperandKind::PhysicalPredicate)
        return predicate_from_physical(static_cast<uint8_t>(operand.id()), operand.inverted(), predicate);
    if (operand.kind() != MachineOperandKind::VirtualPredicate || operand.id() >= assignment.size())
        return false;
    const uint8_t physical = assignment[operand.id()];
    return physical != 0xff && predicate_from_physical(physical, operand.inverted(), predicate);
}

bool resolve_guard(const MachineInstruction &instruction, const std::vector<uint8_t> &assignment,
                   usse::Predicate *predicate) {
    if (!instruction.has_guard()) {
        *predicate = usse::Predicate::Always;
        return true;
    }
    if (instruction.guard_is_physical())
        return predicate_from_physical(static_cast<uint8_t>(instruction.guard_id()),
                                       instruction.guard_inverted(), predicate);
    if (instruction.guard_id() >= assignment.size()) return false;
    const uint8_t physical = assignment[instruction.guard_id()];
    return physical != 0xff && predicate_from_physical(physical, instruction.guard_inverted(), predicate);
}

bool resolve_physical_value(const MachineOperand &operand, MachineType expected,
                            usse::RegisterRef *reg) {
    if (!reg || operand.kind() != MachineOperandKind::PhysicalValue || operand.type() != expected)
        return false;
    *reg = operand.physical_register();
    return reg->bank != usse::RegisterBank::Invalid;
}

} // namespace

MachineOperand MachineOperand::virtual_value(uint32_t id, MachineType type) {
    return pack_operand(MachineOperandKind::VirtualValue, type, id, false);
}

MachineOperand MachineOperand::physical_value(usse::RegisterBank bank, uint8_t num, MachineType type) {
    const uint32_t payload = (static_cast<uint32_t>(bank) << 8) | num;
    return pack_operand(MachineOperandKind::PhysicalValue, type, payload, false);
}

MachineOperand MachineOperand::virtual_predicate(uint32_t id, bool inverted) {
    return pack_operand(MachineOperandKind::VirtualPredicate, MachineType::Invalid, id, inverted);
}

MachineOperand MachineOperand::physical_predicate(uint8_t num, bool inverted) {
    return pack_operand(MachineOperandKind::PhysicalPredicate, MachineType::Invalid, num, inverted);
}

MachineOperand MachineOperand::literal(uint32_t id, MachineType type) {
    return pack_operand(MachineOperandKind::Literal, type, id, false);
}

MachineOperandKind MachineOperand::kind() const {
    return static_cast<MachineOperandKind>((bits >> kKindShift) & 0x7u);
}

MachineType MachineOperand::type() const {
    return static_cast<MachineType>((bits >> kTypeShift) & 0xfu);
}

uint32_t MachineOperand::id() const { return bits & kPayloadMask; }
bool MachineOperand::inverted() const { return (bits & kInvertBit) != 0; }

usse::RegisterRef MachineOperand::physical_register() const {
    if (kind() != MachineOperandKind::PhysicalValue) return {};
    const uint32_t payload = id();
    return {static_cast<usse::RegisterBank>((payload >> 8) & 0x0f), static_cast<uint8_t>(payload)};
}

MachineOpcode MachineInstruction::opcode() const {
    return static_cast<MachineOpcode>(code & 0xffu);
}

uint8_t MachineInstruction::subop() const { return static_cast<uint8_t>(code >> 8); }
bool MachineInstruction::has_guard() const { return (control & 0x8000u) != 0; }
bool MachineInstruction::guard_inverted() const { return (control & 0x4000u) != 0; }
bool MachineInstruction::guard_is_physical() const { return (control & 0x2000u) != 0; }
uint16_t MachineInstruction::guard_id() const { return control & 0x03ffu; }

MachineOperand MachineProgram::make_value(MachineType type) {
    return MachineOperand::virtual_value(next_value_++, type);
}

MachineOperand MachineProgram::make_predicate(bool inverted) {
    if (next_predicate_ >= 1024) return {};
    return MachineOperand::virtual_predicate(next_predicate_++, inverted);
}

MachineOperand MachineProgram::physical(usse::RegisterBank bank, uint8_t num, MachineType type) const {
    return MachineOperand::physical_value(bank, num, type);
}

MachineOperand MachineProgram::literal_u32(uint32_t value) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(value);
    return MachineOperand::literal(static_cast<uint32_t>(literals_.size() - 1), MachineType::U32);
}

bool MachineProgram::append(MachineOpcode opcode, uint8_t subop, MachineOperand dst,
                            MachineOperand src0, MachineOperand src1, MachineOperand guard) {
    if (!descriptor(opcode)) return false;
    MachineInstruction instruction{};
    instruction.code = static_cast<uint16_t>(static_cast<uint8_t>(opcode)) |
        (static_cast<uint16_t>(subop) << 8);
    instruction.dst = dst;
    instruction.src0 = src0;
    instruction.src1 = src1;
    if (guard.kind() != MachineOperandKind::None) {
        if (!is_predicate_operand(guard) || guard.id() >= 1024) return false;
        instruction.control = 0x8000u | (guard.inverted() ? 0x4000u : 0u) |
            (guard.kind() == MachineOperandKind::PhysicalPredicate ? 0x2000u : 0u) |
            static_cast<uint16_t>(guard.id());
    }
    instructions_.push_back(instruction);
    return true;
}

bool compile_machine_program(const MachineProgram &program, MachineCompileResult &out) {
    out = {};
    std::vector<PredicateInterval> intervals(program.predicate_count());

    for (uint32_t index = 0; index < program.instructions().size(); ++index) {
        const auto &instruction = program.instructions()[index];
        const OpcodeDesc *desc = descriptor(instruction.opcode());
        if (!desc) { out.error = "unknown machine opcode"; return false; }
        const MachineOperand operands[] = {instruction.dst, instruction.src0, instruction.src1};
        for (size_t slot = 0; slot < 3; ++slot) {
            const auto role = desc->roles[slot];
            const auto &operand = operands[slot];
            if (role == OperandRole::None) {
                if (operand.kind() != MachineOperandKind::None) {
                    out.error = std::string(desc->name) + " has an unexpected operand";
                    return false;
                }
                continue;
            }
            if ((role == OperandRole::ValueUse || role == OperandRole::ValueDef) && !is_value_operand(operand)) {
                out.error = std::string(desc->name) + " requires a value operand";
                return false;
            }
            if (role == OperandRole::PredicateDef) {
                if (operand.kind() != MachineOperandKind::VirtualPredicate || operand.inverted() ||
                    operand.id() >= intervals.size()) {
                    out.error = std::string(desc->name) + " requires a virtual predicate destination";
                    return false;
                }
                auto &interval = intervals[operand.id()];
                if (interval.defined) { out.error = "virtual predicate defined twice"; return false; }
                interval.defined = true;
                // Uses happen before defs inside one instruction. Encoding the
                // timeline as 2*i (read) / 2*i+1 (write) lets a dead predicate
                // be overwritten by the instruction that consumes it.
                interval.start = 2 * index + 1;
                interval.end = interval.start + 1;
            } else if (role == OperandRole::PredicateUse) {
                if (!is_predicate_operand(operand)) {
                    out.error = std::string(desc->name) + " requires a predicate operand";
                    return false;
                }
                const uint8_t allowed = allowed_predicates(*desc, operand);
                if (operand.kind() == MachineOperandKind::PhysicalPredicate) {
                    if (operand.id() >= 4 || !(allowed & (1u << operand.id()))) {
                        out.error = "physical predicate is not encodable for opcode";
                        return false;
                    }
                } else {
                    if (operand.id() >= intervals.size() || !intervals[operand.id()].defined) {
                        out.error = "virtual predicate used before definition";
                        return false;
                    }
                    auto &interval = intervals[operand.id()];
                    interval.end = std::max(interval.end, 2 * index + 1);
                    interval.allowed &= allowed;
                }
            }
        }

        if (instruction.has_guard()) {
            const uint8_t allowed = allowed_guard_predicates(instruction);
            if (instruction.guard_is_physical()) {
                if (instruction.guard_id() >= 4 || !(allowed & (1u << instruction.guard_id()))) {
                    out.error = "physical guard predicate is not encodable";
                    return false;
                }
            } else {
                if (instruction.guard_id() >= intervals.size() || !intervals[instruction.guard_id()].defined) {
                    out.error = "guard predicate used before definition";
                    return false;
                }
                auto &interval = intervals[instruction.guard_id()];
                interval.end = std::max(interval.end, 2 * index + 1);
                interval.allowed &= allowed;
            }
        }
    }

    out.predicate_registers.assign(program.predicate_count(), 0xff);
    std::vector<uint32_t> order;
    for (uint32_t id = 0; id < intervals.size(); ++id)
        if (intervals[id].defined) order.push_back(id);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return intervals[a].start < intervals[b].start;
    });

    for (uint32_t id : order) {
        const auto &current = intervals[id];
        uint8_t used = 0;
        for (uint32_t other : order) {
            if (other == id || out.predicate_registers[other] == 0xff) continue;
            const auto &prior = intervals[other];
            if (prior.start < current.end && current.start < prior.end)
                used |= static_cast<uint8_t>(1u << out.predicate_registers[other]);
        }
        const uint8_t available = static_cast<uint8_t>(current.allowed & ~used & 0x0f);
        if (!available) { out.error = "predicate register pressure exceeds encodable hardware set"; return false; }
        for (uint8_t reg = 0; reg < 4; ++reg) {
            if (available & (1u << reg)) {
                out.predicate_registers[id] = reg;
                break;
            }
        }
    }

    ProgramBuilder builder;
    for (const auto &instruction : program.instructions()) {
        usse::Predicate guard = usse::Predicate::Always;
        if (!resolve_guard(instruction, out.predicate_registers, &guard)) {
            out.error = "failed to resolve guard predicate";
            return false;
        }
        switch (instruction.opcode()) {
        case MachineOpcode::Compare: {
            if (instruction.subop() > static_cast<uint8_t>(usse::CompareOp::GreaterEqual)) {
                out.error = "invalid compare subop";
                return false;
            }
            usse::VtstSemantic compare{};
            if (!resolve_physical_value(instruction.src0, MachineType::U32, &compare.lhs) ||
                !resolve_physical_value(instruction.src1, MachineType::U32, &compare.rhs)) {
                out.error = "initial machine compare requires physical U32 sources";
                return false;
            }
            if (instruction.dst.id() >= out.predicate_registers.size() ||
                out.predicate_registers[instruction.dst.id()] == 0xff) {
                out.error = "compare predicate was not allocated";
                return false;
            }
            compare.predicate = guard;
            compare.op = static_cast<usse::CompareOp>(instruction.subop());
            compare.predicate_destination = out.predicate_registers[instruction.dst.id()];
            if (!builder.vtst(compare)) { out.error = "failed to encode machine compare"; return false; }
            break;
        }
        case MachineOpcode::Kill: {
            usse::Predicate predicate{};
            if (!resolve_predicate(instruction, instruction.src0, out.predicate_registers, &predicate)) {
                out.error = "failed to resolve kill predicate";
                return false;
            }
            if (guard != usse::Predicate::Always) {
                out.error = "guarded kill is not in the validated machine subset";
                return false;
            }
            if (!builder.kill(predicate)) { out.error = "failed to encode machine kill"; return false; }
            break;
        }
        }
    }
    out.words = builder.words();
    return true;
}

} // namespace vsc::backend
