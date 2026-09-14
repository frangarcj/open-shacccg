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
    ValueUpdate,
    ValuePairUse,
    LabelUse,
    PredicateUse,
    PredicateDef,
};

struct OpcodeDesc {
    const char *name;
    std::array<OperandRole, 3> roles;
    uint8_t predicate_use_mask;
};

constexpr OpcodeDesc kOpcodeDesc[] = {
#define VSC_MACHINE_OP(name, label, dst, src0, src1, predicates) \
    {label, {OperandRole::dst, OperandRole::src0, OperandRole::src1}, predicates},
#include "backend/machine_ops.inc"
#undef VSC_MACHINE_OP
};
static_assert(std::size(kOpcodeDesc) == static_cast<size_t>(MachineOpcode::Count));

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
    if (instruction.opcode() == MachineOpcode::Branch) return 0x01;
    if (!instruction.guard_inverted()) return 0x0f;
    return 0x03;
}

bool is_value_operand(const MachineOperand &operand) {
    return operand.kind() == MachineOperandKind::VirtualValue ||
        operand.kind() == MachineOperandKind::PhysicalValue ||
        operand.kind() == MachineOperandKind::Literal;
}

bool is_value_pair_operand(const MachineOperand &operand) {
    return operand.kind() == MachineOperandKind::VirtualPair;
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

struct ValueInterval {
    bool defined = false;
    uint32_t start = 0;
    uint32_t end = 0;
    MachineType type = MachineType::Invalid;
};

bool ranges_overlap(const MachineLiveRange &a, const MachineLiveRange &b) {
    return a.start < b.end && b.start < a.end;
}

bool registers_overlap(usse::RegisterRef a, uint8_t a_width,
                       usse::RegisterRef b, uint8_t b_width) {
    if (a.bank != b.bank) return false;
    const uint16_t a_end = static_cast<uint16_t>(a.num) + a_width;
    const uint16_t b_end = static_cast<uint16_t>(b.num) + b_width;
    return a.num < b_end && b.num < a_end;
}

bool valid_live_range(const MachineLiveRange &range) {
    if (range.type == MachineType::Invalid || range.start >= range.end || range.width == 0) return false;
    switch (range.reg_class) {
    case MachineRegisterClass::ScalarTemp:
        return (range.type == MachineType::U32 || range.type == MachineType::U16 ||
                range.type == MachineType::S32) && range.width == 1;
    case MachineRegisterClass::FloatTemp:
        return (range.type == MachineType::F32 || range.type == MachineType::F16) &&
            (range.width == 1 || range.width == 2);
    case MachineRegisterClass::Gpi:
        return range.type == MachineType::F32 && range.width == 1;
    case MachineRegisterClass::VmadAccumulator:
        return range.type == MachineType::F32 && range.width == 1;
    }
    return false;
}

bool valid_value_desc(const MachineValueDesc &desc) {
    return valid_live_range({0, 1, desc.type, desc.reg_class, desc.order, desc.width});
}

bool reserve_physical(const MachineOperand &operand, std::array<bool, 128> &reserved,
                      std::string &error) {
    if (operand.kind() != MachineOperandKind::PhysicalValue) return true;
    const auto reg = operand.physical_register();
    if (reg.bank == usse::RegisterBank::Invalid) {
        error = "invalid physical value register";
        return false;
    }
    if (reg.bank == usse::RegisterBank::Temp) {
        if (reg.num >= reserved.size()) {
            error = "physical TEMP register exceeds current machine subset";
            return false;
        }
        reserved[reg.num] = true;
    }
    return true;
}

template <typename Reserved>
bool allocate_live_ranges(const std::vector<MachineLiveRange> &ranges,
                          const Reserved &reserved,
                          std::vector<usse::RegisterRef> &assignment,
                          std::string &error) {
    assignment.assign(ranges.size(), {});
    std::vector<uint32_t> order;
    order.reserve(ranges.size());
    for (uint32_t id = 0; id < ranges.size(); ++id) {
        if (!valid_live_range(ranges[id])) {
            error = "invalid machine live range";
            return false;
        }
        order.push_back(id);
    }
    std::stable_sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return ranges[a].start < ranges[b].start;
    });

    auto available = [&](uint32_t id, uint8_t first) {
        const auto &range = ranges[id];
        if (static_cast<uint16_t>(first) + range.width > reserved.size()) return false;
        for (uint16_t i = first; i < static_cast<uint16_t>(first) + range.width; ++i)
            if (reserved[i]) return false;
        const usse::RegisterRef candidate{usse::RegisterBank::Temp, first};
        for (uint32_t other : order) {
            if (other == id || assignment[other].bank == usse::RegisterBank::Invalid) continue;
            if (ranges_overlap(range, ranges[other]) &&
                registers_overlap(candidate, range.width, assignment[other], ranges[other].width))
                return false;
        }
        return true;
    };

    for (uint32_t id : order) {
        const auto &range = ranges[id];
        uint8_t min_reg = 0;
        uint8_t max_reg = 127;
        uint8_t stride = 1;
        switch (range.reg_class) {
        case MachineRegisterClass::ScalarTemp:
            max_reg = static_cast<uint8_t>(128 - range.width);
            break;
        case MachineRegisterClass::FloatTemp:
            min_reg = 4;
            max_reg = 60;
            stride = range.width == 2 ? 2 : 1;
            break;
        case MachineRegisterClass::Gpi:
            min_reg = 124;
            max_reg = 127;
            break;
        case MachineRegisterClass::VmadAccumulator:
            min_reg = max_reg = 61;
            break;
        }

        bool found = false;
        if (range.order == MachineRegisterOrder::Low) {
            for (uint16_t reg = min_reg; reg <= max_reg; reg += stride) {
                if (available(id, static_cast<uint8_t>(reg))) {
                    assignment[id] = {usse::RegisterBank::Temp, static_cast<uint8_t>(reg)};
                    found = true;
                    break;
                }
            }
        } else {
            for (int reg = max_reg; reg >= min_reg; reg -= stride) {
                if (available(id, static_cast<uint8_t>(reg))) {
                    assignment[id] = {usse::RegisterBank::Temp, static_cast<uint8_t>(reg)};
                    found = true;
                    break;
                }
            }
        }
        if (!found) {
            error = "machine register pressure exceeds the validated bank subset";
            return false;
        }
    }
    error.clear();
    return true;
}

template <size_t RegisterCount, typename Interval, typename Allowed>
bool allocate_intervals(const std::vector<Interval> &intervals,
                        const std::array<bool, RegisterCount> &reserved,
                        Allowed allowed,
                        std::vector<uint8_t> &assignment) {
    assignment.assign(intervals.size(), 0xff);
    std::vector<uint32_t> order;
    for (uint32_t id = 0; id < intervals.size(); ++id)
        if (intervals[id].defined) order.push_back(id);
    std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) {
        return intervals[a].start < intervals[b].start;
    });

    for (uint32_t id : order) {
        const auto &current = intervals[id];
        for (uint16_t reg = 0; reg < RegisterCount; ++reg) {
            if (reserved[reg] || !allowed(current, static_cast<uint8_t>(reg))) continue;
            bool busy = false;
            for (uint32_t other : order) {
                if (other == id || assignment[other] != reg) continue;
                const auto &prior = intervals[other];
                if (prior.start < current.end && current.start < prior.end) {
                    busy = true;
                    break;
                }
            }
            if (!busy) {
                assignment[id] = static_cast<uint8_t>(reg);
                break;
            }
        }
        if (assignment[id] == 0xff) return false;
    }
    return true;
}

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

bool resolve_register_value(const MachineOperand &operand, MachineType expected,
                            const std::vector<usse::RegisterRef> &assignment,
                            usse::RegisterRef *reg) {
    if (!reg || operand.type() != expected) return false;
    if (operand.kind() == MachineOperandKind::PhysicalValue) {
        *reg = operand.physical_register();
        return reg->bank != usse::RegisterBank::Invalid;
    }
    if (operand.kind() != MachineOperandKind::VirtualValue || operand.id() >= assignment.size())
        return false;
    *reg = assignment[operand.id()];
    return reg->bank != usse::RegisterBank::Invalid;
}

bool resolve_virtual_pair(const MachineOperand &operand, MachineType expected,
                          const std::vector<usse::RegisterRef> &assignment,
                          usse::RegisterRef *first, usse::RegisterRef *second) {
    if (!first || !second || operand.kind() != MachineOperandKind::VirtualPair ||
        operand.type() != expected) return false;
    const uint16_t a = operand.pair_first();
    const uint16_t b = operand.pair_second();
    if (a >= assignment.size() || b >= assignment.size()) return false;
    *first = assignment[a];
    *second = assignment[b];
    return first->bank != usse::RegisterBank::Invalid && second->bank != usse::RegisterBank::Invalid;
}

MachineType data_type_machine_type(usse::DataType type) {
    switch (type) {
    case usse::DataType::F32: return MachineType::F32;
    case usse::DataType::F16: return MachineType::F16;
    default: return MachineType::Invalid;
    }
}

MachineType pack_format_machine_type(usse::PackFormat format) {
    switch (format) {
    case usse::PackFormat::F32: return MachineType::F32;
    case usse::PackFormat::F16: return MachineType::F16;
    case usse::PackFormat::S16: return MachineType::S32;
    default: return MachineType::Invalid;
    }
}

bool uses_instruction_config(MachineOpcode opcode) {
    return opcode == MachineOpcode::Move || opcode == MachineOpcode::MoveUpdate || opcode == MachineOpcode::Pack ||
        opcode == MachineOpcode::PackSwizzle || opcode == MachineOpcode::PackValue || opcode == MachineOpcode::Vector ||
        opcode == MachineOpcode::Vmad;
}

} // namespace

bool allocate_machine_live_ranges(const std::vector<MachineLiveRange> &ranges,
                                  std::vector<usse::RegisterRef> &assignment,
                                  std::string &error) {
    std::array<bool, 128> reserved{};
    return allocate_live_ranges(ranges, reserved, assignment, error);
}

MachineOperand MachineOperand::virtual_value(uint32_t id, MachineType type) {
    return pack_operand(MachineOperandKind::VirtualValue, type, id, false);
}

MachineOperand MachineOperand::physical_value(usse::RegisterBank bank, uint8_t num, MachineType type,
                                              uint8_t component) {
    if (component != 0xff && component >= 4) return {};
    const uint32_t payload = (static_cast<uint32_t>(bank) << 8) | num |
        (component != 0xff ? (uint32_t{1} << 14) | (static_cast<uint32_t>(component) << 12) : 0u);
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

MachineOperand MachineOperand::virtual_pair(uint16_t first, uint16_t second, MachineType type) {
    if (first >= 4096 || second >= 4096) return {};
    return pack_operand(MachineOperandKind::VirtualPair, type,
                        static_cast<uint32_t>(first) | (static_cast<uint32_t>(second) << 12), false);
}

MachineOperand MachineOperand::label(uint32_t id) {
    return pack_operand(MachineOperandKind::Label, MachineType::Invalid, id, false);
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

uint8_t MachineOperand::physical_component() const {
    if (kind() != MachineOperandKind::PhysicalValue || !(id() & (uint32_t{1} << 14))) return 0xff;
    return static_cast<uint8_t>((id() >> 12) & 0x03u);
}

uint16_t MachineOperand::pair_first() const {
    return kind() == MachineOperandKind::VirtualPair ? static_cast<uint16_t>(id() & 0x0fffu) : 0xffffu;
}

uint16_t MachineOperand::pair_second() const {
    return kind() == MachineOperandKind::VirtualPair ? static_cast<uint16_t>((id() >> 12) & 0x0fffu) : 0xffffu;
}

MachineOpcode MachineInstruction::opcode() const {
    return static_cast<MachineOpcode>(code & 0xffu);
}

uint8_t MachineInstruction::subop() const { return static_cast<uint8_t>(code >> 8); }
bool MachineInstruction::has_guard() const { return (control & 0x8000u) != 0; }
bool MachineInstruction::guard_inverted() const { return (control & 0x4000u) != 0; }
bool MachineInstruction::guard_is_physical() const { return (control & 0x2000u) != 0; }
uint16_t MachineInstruction::guard_id() const { return control & 0x03ffu; }
uint16_t MachineInstruction::config() const { return control & 0x1fffu; }

MachineOperand MachineProgram::make_value(MachineType type) {
    switch (type) {
    case MachineType::F32:
    case MachineType::F16:
        return make_value(type, MachineRegisterClass::FloatTemp, 1, MachineRegisterOrder::Low);
    case MachineType::U32:
    case MachineType::U16:
    case MachineType::S32:
        return make_value(type, MachineRegisterClass::ScalarTemp, 1, MachineRegisterOrder::Low);
    case MachineType::Invalid:
        return {};
    }
    return {};
}

MachineOperand MachineProgram::make_value(MachineType type, MachineRegisterClass reg_class,
                                          uint8_t width, MachineRegisterOrder order) {
    if (value_descs_.size() > kPayloadMask) return {};
    const MachineValueDesc desc{type, reg_class, order, width};
    if (!valid_value_desc(desc)) return {};
    const uint32_t id = static_cast<uint32_t>(value_descs_.size());
    value_descs_.push_back(desc);
    return MachineOperand::virtual_value(id, type);
}

MachineOperand MachineProgram::make_predicate(bool inverted) {
    if (next_predicate_ >= 1024) return {};
    return MachineOperand::virtual_predicate(next_predicate_++, inverted);
}

MachineOperand MachineProgram::physical(usse::RegisterBank bank, uint8_t num, MachineType type,
                                        uint8_t component) const {
    return MachineOperand::physical_value(bank, num, type, component);
}

MachineOperand MachineProgram::physical(usse::RegisterRef reg, MachineType type, uint8_t component) const {
    return MachineOperand::physical_value(reg.bank, reg.num, type, component);
}

MachineOperand MachineProgram::literal_u32(uint32_t value) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(value);
    return MachineOperand::literal(static_cast<uint32_t>(literals_.size() - 1), MachineType::U32);
}

MachineOperand MachineProgram::literal_s32(int32_t value) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(static_cast<uint32_t>(value));
    return MachineOperand::literal(static_cast<uint32_t>(literals_.size() - 1), MachineType::S32);
}

MachineOperand MachineProgram::pair(MachineOperand first, MachineOperand second) const {
    if (first.kind() != MachineOperandKind::VirtualValue || second.kind() != MachineOperandKind::VirtualValue ||
        first.type() == MachineType::Invalid || first.type() != second.type() ||
        first.id() >= value_descs_.size() || second.id() >= value_descs_.size() ||
        first.id() >= 4096 || second.id() >= 4096) return {};
    return MachineOperand::virtual_pair(static_cast<uint16_t>(first.id()),
                                        static_cast<uint16_t>(second.id()), first.type());
}

MachineOperand MachineProgram::make_label() {
    if (labels_.size() > kPayloadMask) return {};
    const uint32_t id = static_cast<uint32_t>(labels_.size());
    labels_.push_back(std::numeric_limits<uint32_t>::max());
    return MachineOperand::label(id);
}

bool MachineProgram::bind_label(MachineOperand label) {
    if (label.kind() != MachineOperandKind::Label || label.id() >= labels_.size() ||
        labels_[label.id()] != std::numeric_limits<uint32_t>::max()) return false;
    labels_[label.id()] = static_cast<uint32_t>(instructions_.size());
    return true;
}

bool MachineProgram::branch(MachineOperand target, MachineOperand guard) {
    if (target.kind() != MachineOperandKind::Label) return false;
    return append(MachineOpcode::Branch, 0, {}, target, {}, guard);
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

bool MachineProgram::append_config(MachineOpcode opcode, uint8_t subop, uint16_t config,
                                   MachineOperand dst, MachineOperand src0, MachineOperand src1) {
    if (!descriptor(opcode) || !uses_instruction_config(opcode) || (config & 0xe000u)) return false;
    MachineInstruction instruction{};
    instruction.code = static_cast<uint16_t>(static_cast<uint8_t>(opcode)) |
        (static_cast<uint16_t>(subop) << 8);
    instruction.control = config;
    instruction.dst = dst;
    instruction.src0 = src0;
    instruction.src1 = src1;
    instructions_.push_back(instruction);
    return true;
}

bool compile_machine_program(const MachineProgram &program, MachineCompileResult &out) {
    out = {};
    if (program.instructions().size() > (std::numeric_limits<uint16_t>::max() - 2u) / 2u) {
        out.error = "machine program exceeds compact live-range timeline";
        return false;
    }
    std::vector<PredicateInterval> predicate_intervals(program.predicate_count());
    std::vector<ValueInterval> value_intervals(program.value_count());
    std::array<bool, 128> reserved_temps{};

    for (uint32_t id = 0; id < program.value_descs().size(); ++id) {
        const auto &desc = program.value_descs()[id];
        if (!valid_value_desc(desc)) {
            out.error = "invalid machine value descriptor";
            return false;
        }
        value_intervals[id].type = desc.type;
    }

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
            if (role == OperandRole::ValueDef) {
                if (operand.kind() == MachineOperandKind::PhysicalValue) {
                    if (operand.type() == MachineType::Invalid ||
                        !reserve_physical(operand, reserved_temps, out.error)) return false;
                    continue;
                }
                if (operand.kind() != MachineOperandKind::VirtualValue || operand.type() == MachineType::Invalid ||
                    operand.id() >= value_intervals.size() ||
                    program.value_descs()[operand.id()].type != operand.type()) {
                    out.error = std::string(desc->name) + " requires a valid value destination";
                    return false;
                }
                auto &interval = value_intervals[operand.id()];
                if (interval.defined) { out.error = "virtual value defined twice"; return false; }
                interval.defined = true;
                interval.start = 2 * index + 1;
                interval.end = interval.start + 1;
            } else if (role == OperandRole::ValueUpdate) {
                if (operand.kind() != MachineOperandKind::VirtualValue || operand.type() == MachineType::Invalid ||
                    operand.id() >= value_intervals.size() || !value_intervals[operand.id()].defined ||
                    program.value_descs()[operand.id()].type != operand.type()) {
                    out.error = std::string(desc->name) + " requires an already-defined mutable value";
                    return false;
                }
                auto &interval = value_intervals[operand.id()];
                interval.end = std::max(interval.end, 2 * index + 2);
            } else if (role == OperandRole::ValueUse) {
                if (!is_value_operand(operand)) {
                    out.error = std::string(desc->name) + " requires a value operand";
                    return false;
                }
                if (operand.kind() == MachineOperandKind::VirtualValue) {
                    if (operand.id() >= value_intervals.size() || !value_intervals[operand.id()].defined) {
                        out.error = "virtual value used before definition";
                        return false;
                    }
                    auto &interval = value_intervals[operand.id()];
                    if (interval.type != operand.type()) { out.error = "virtual value type mismatch"; return false; }
                    interval.end = std::max(interval.end, 2 * index + 1);
                } else if (operand.kind() == MachineOperandKind::PhysicalValue) {
                    if (!reserve_physical(operand, reserved_temps, out.error)) return false;
                } else if (operand.kind() == MachineOperandKind::Literal) {
                    if (operand.id() >= program.literals().size()) {
                        out.error = "literal operand is out of range";
                        return false;
                    }
                }
            } else if (role == OperandRole::ValuePairUse) {
                if (!is_value_pair_operand(operand) || operand.type() == MachineType::Invalid) {
                    out.error = std::string(desc->name) + " requires a virtual value pair";
                    return false;
                }
                const uint16_t ids[] = {operand.pair_first(), operand.pair_second()};
                for (uint16_t id : ids) {
                    if (id >= value_intervals.size() || !value_intervals[id].defined ||
                        value_intervals[id].type != operand.type()) {
                        out.error = "virtual value pair uses an unresolved or mismatched value";
                        return false;
                    }
                    value_intervals[id].end = std::max(value_intervals[id].end, 2 * index + 1);
                }
            } else if (role == OperandRole::LabelUse) {
                if (operand.kind() != MachineOperandKind::Label || operand.id() >= program.labels().size()) {
                    out.error = std::string(desc->name) + " requires a valid machine label";
                    return false;
                }
            } else if (role == OperandRole::PredicateDef) {
                if (operand.kind() != MachineOperandKind::VirtualPredicate || operand.inverted() ||
                    operand.id() >= predicate_intervals.size()) {
                    out.error = std::string(desc->name) + " requires a virtual predicate destination";
                    return false;
                }
                auto &interval = predicate_intervals[operand.id()];
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
                    if (operand.id() >= predicate_intervals.size() || !predicate_intervals[operand.id()].defined) {
                        out.error = "virtual predicate used before definition";
                        return false;
                    }
                    auto &interval = predicate_intervals[operand.id()];
                    interval.end = std::max(interval.end, 2 * index + 1);
                    interval.allowed &= allowed;
                }
            }
        }

        if (uses_instruction_config(instruction.opcode()) && instruction.has_guard()) {
            out.error = "configured float machine operation cannot carry a guard predicate yet";
            return false;
        }
        if (instruction.has_guard()) {
            const uint8_t allowed = allowed_guard_predicates(instruction);
            if (instruction.guard_is_physical()) {
                if (instruction.guard_id() >= 4 || !(allowed & (1u << instruction.guard_id()))) {
                    out.error = "physical guard predicate is not encodable";
                    return false;
                }
            } else {
                if (instruction.guard_id() >= predicate_intervals.size() || !predicate_intervals[instruction.guard_id()].defined) {
                    out.error = "guard predicate used before definition";
                    return false;
                }
                auto &interval = predicate_intervals[instruction.guard_id()];
                interval.end = std::max(interval.end, 2 * index + 1);
                interval.allowed &= allowed;
            }
        }
        if (instruction.opcode() == MachineOpcode::LoopIncrement)
            reserved_temps[1] = true;
    }

    std::array<bool, 4> no_reserved_predicates{};
    if (!allocate_intervals<4>(predicate_intervals, no_reserved_predicates,
                               [](const PredicateInterval &interval, uint8_t reg) {
                                   return (interval.allowed & (1u << reg)) != 0;
                               }, out.predicate_registers)) {
        out.error = "predicate register pressure exceeds encodable hardware set";
        return false;
    }

    std::vector<MachineLiveRange> value_ranges;
    std::vector<uint32_t> range_for_value(value_intervals.size(), std::numeric_limits<uint32_t>::max());
    for (uint32_t id = 0; id < value_intervals.size(); ++id) {
        if (!value_intervals[id].defined) continue;
        const auto &desc = program.value_descs()[id];
        range_for_value[id] = static_cast<uint32_t>(value_ranges.size());
        value_ranges.push_back({static_cast<uint16_t>(value_intervals[id].start),
                                static_cast<uint16_t>(value_intervals[id].end),
                                desc.type, desc.reg_class, desc.order, desc.width});
    }
    std::vector<usse::RegisterRef> allocated_values;
    std::string allocation_error;
    if (!allocate_live_ranges(value_ranges, reserved_temps, allocated_values, allocation_error)) {
        out.error = allocation_error;
        return false;
    }
    out.value_registers.assign(program.value_count(), {});
    for (uint32_t id = 0; id < value_intervals.size(); ++id) {
        if (range_for_value[id] == std::numeric_limits<uint32_t>::max()) continue;
        out.value_registers[id] = allocated_values[range_for_value[id]];
    }

    // Labels are bound to Machine IR instruction positions, but BR offsets are
    // measured in emitted USSE instructions. Account for zero- and multi-word
    // pseudo-ops when resolving the final signed delta.
    std::vector<uint32_t> word_positions(program.instructions().size() + 1, 0);
    for (uint32_t i = 0; i < program.instructions().size(); ++i) {
        uint32_t words=1;
        if (program.instructions()[i].opcode()==MachineOpcode::DependentSample) words=0;
        else if (program.instructions()[i].opcode()==MachineOpcode::LoopIncrement) words=2;
        else if (program.instructions()[i].opcode()==MachineOpcode::DivF32) {
            const uint8_t components=program.instructions()[i].subop();
            words=(components>=1 && components<=4) ? static_cast<uint32_t>(components+2) : 1;
        } else if (program.instructions()[i].opcode()==MachineOpcode::DotSplatF32) words=3;
        else if (program.instructions()[i].opcode()==MachineOpcode::F32ToS32Color) words=3;
        word_positions[i + 1] = word_positions[i] + words;
    }
    for (uint32_t position : program.labels()) {
        if (position == std::numeric_limits<uint32_t>::max() || position > program.instructions().size()) {
            out.error = "machine branch label is unbound or out of range";
            return false;
        }
    }

    ProgramBuilder builder;
    for (uint32_t instruction_index = 0; instruction_index < program.instructions().size(); ++instruction_index) {
        const auto &instruction = program.instructions()[instruction_index];
        usse::Predicate guard = usse::Predicate::Always;
        if (!resolve_guard(instruction, out.predicate_registers, &guard)) {
            out.error = "failed to resolve guard predicate";
            return false;
        }
        switch (instruction.opcode()) {
        case MachineOpcode::Phase:
            if (guard != usse::Predicate::Always || !builder.phase()) {
                out.error = "failed to encode machine PHAS";
                return false;
            }
            break;
        case MachineOpcode::Nop:
            if (guard != usse::Predicate::Always || !builder.nop()) {
                out.error = "failed to encode machine NOP";
                return false;
            }
            break;
        case MachineOpcode::Emit:
            if (guard != usse::Predicate::Always || !builder.emit()) {
                out.error = "failed to encode machine EMIT";
                return false;
            }
            break;
        case MachineOpcode::Move:
        case MachineOpcode::MoveUpdate: {
            const auto data_type = static_cast<usse::DataType>(instruction.subop());
            const MachineType expected = data_type_machine_type(data_type);
            const uint16_t config = instruction.config();
            if (expected == MachineType::Invalid || (config & ~0x0fffu)) {
                out.error = "machine move is outside the validated F32/F16 subset";
                return false;
            }
            usse::VmovSemantic move{};
            if (!resolve_register_value(instruction.dst, expected, out.value_registers, &move.dst) ||
                !resolve_register_value(instruction.src0, expected, out.value_registers, &move.src)) {
                out.error = "machine move operands are not register-backed values of the requested type";
                return false;
            }
            move.data_type = data_type;
            move.dest_mask = static_cast<uint8_t>(config & 0x0f);
            move.swizzle = static_cast<uint8_t>((config >> 4) & 0x0f);
            move.repeat_count = static_cast<uint8_t>((config >> 8) & 0x03);
            move.skip_invalid = (config & 0x0400u) != 0;
            move.no_schedule = (config & 0x0800u) != 0;
            if (!builder.instruction(move)) { out.error = "failed to encode machine VMOV"; return false; }
            break;
        }
        case MachineOpcode::Pack: {
            if (instruction.subop() & 0xc0u) { out.error = "invalid machine pack format bits"; return false; }
            const auto src_format = static_cast<usse::PackFormat>(instruction.subop() & 0x07u);
            const auto dst_format = static_cast<usse::PackFormat>((instruction.subop() >> 3) & 0x07u);
            const MachineType src_type = pack_format_machine_type(src_format);
            const MachineType dst_type = pack_format_machine_type(dst_format);
            const uint16_t config = instruction.config();
            if (src_type == MachineType::Invalid || dst_type == MachineType::Invalid || (config & ~0x007fu)) {
                out.error = "machine pack is outside the validated F32/F16 subset";
                return false;
            }
            usse::VpckSemantic pack{};
            if (!resolve_register_value(instruction.dst, dst_type, out.value_registers, &pack.dst) ||
                !resolve_register_value(instruction.src0, src_type, out.value_registers, &pack.src1) ||
                !resolve_register_value(instruction.src1, src_type, out.value_registers, &pack.src2)) {
                out.error = "machine pack operands are not register-backed values of the requested formats";
                return false;
            }
            pack.src_format = src_format;
            pack.dst_format = dst_format;
            pack.dest_mask = static_cast<uint8_t>(config & 0x0f);
            pack.skip_invalid = (config & 0x0010u) != 0;
            pack.no_schedule = (config & 0x0020u) != 0;
            pack.end = (config & 0x0040u) != 0;
            if (!builder.instruction(pack)) { out.error = "failed to encode machine VPCK"; return false; }
            break;
        }
        case MachineOpcode::PackSwizzle: {
            const uint16_t config=instruction.config();
            if (config & ~0x007fu) { out.error="invalid machine pack-swizzle config"; return false; }
            usse::VpckSemantic pack{};
            if (!resolve_register_value(instruction.dst,MachineType::F16,out.value_registers,&pack.dst) ||
                !resolve_register_value(instruction.src0,MachineType::F32,out.value_registers,&pack.src1) ||
                !resolve_register_value(instruction.src1,MachineType::F32,out.value_registers,&pack.src2)) {
                out.error="machine pack-swizzle requires F16 destination and F32 source pair";
                return false;
            }
            pack.src_format=usse::PackFormat::F32;
            pack.dst_format=usse::PackFormat::F16;
            pack.dest_mask=static_cast<uint8_t>(config&0x0f);
            pack.skip_invalid=(config&0x0010u)!=0;
            pack.no_schedule=(config&0x0020u)!=0;
            pack.end=(config&0x0040u)!=0;
            for (uint8_t lane=0;lane<4;++lane)
                pack.components[lane]=static_cast<uint8_t>((instruction.subop()>>(2*lane))&0x03u);
            if (!builder.instruction(pack)) { out.error="failed to encode machine swizzled VPCK"; return false; }
            break;
        }
        case MachineOpcode::PackValue: {
            if (instruction.subop() != machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16)) {
                out.error = "machine pack-value currently supports only F32x4 to F16x4";
                return false;
            }
            const uint16_t config = instruction.config();
            if (config & ~0x007fu) { out.error = "invalid machine pack-value config"; return false; }
            usse::VpckSemantic pack{};
            if (!resolve_register_value(instruction.dst, MachineType::F16, out.value_registers, &pack.dst) ||
                !resolve_register_value(instruction.src0, MachineType::F32, out.value_registers, &pack.src1)) {
                out.error = "machine pack-value requires F16 destination and F32 source";
                return false;
            }
            if (instruction.src0.kind() == MachineOperandKind::VirtualValue) {
                if (instruction.src0.id() >= program.value_descs().size() ||
                    program.value_descs()[instruction.src0.id()].width != 2) {
                    out.error = "machine F32x4 pack source must occupy a validated register pair";
                    return false;
                }
            }
            if (instruction.dst.kind() == MachineOperandKind::VirtualValue &&
                (instruction.dst.id() >= program.value_descs().size() ||
                 program.value_descs()[instruction.dst.id()].width != 1)) {
                out.error = "machine F16x4 pack destination must occupy one validated register";
                return false;
            }
            if (pack.src1.num >= 127) { out.error = "machine pack-value source register overflows"; return false; }
            pack.src2 = {pack.src1.bank, static_cast<uint8_t>(pack.src1.num + 1)};
            pack.src_format = usse::PackFormat::F32;
            pack.dst_format = usse::PackFormat::F16;
            pack.dest_mask = static_cast<uint8_t>(config & 0x0f);
            pack.skip_invalid = (config & 0x0010u) != 0;
            pack.no_schedule = (config & 0x0020u) != 0;
            pack.end = (config & 0x0040u) != 0;
            if (!builder.instruction(pack)) { out.error = "failed to encode machine pack-value VPCK"; return false; }
            break;
        }
        case MachineOpcode::Vector: {
            if (instruction.subop() > static_cast<uint8_t>(usse::VectorOp::Dot)) {
                out.error = "invalid machine vector operation";
                return false;
            }
            const uint16_t config = instruction.config();
            if (config & ~0x07ffu) { out.error = "invalid machine vector config"; return false; }
            const auto swizzle = static_cast<MachineVectorSwizzle>((config >> 4) & 0x03u);
            if (swizzle != MachineVectorSwizzle::Identity &&
                swizzle != MachineVectorSwizzle::PositionXY11 &&
                swizzle != MachineVectorSwizzle::Source2YYYY) {
                out.error = "machine vector swizzle profile is not validated";
                return false;
            }
            usse::V32NmadSemantic op{};
            if (!resolve_register_value(instruction.dst, MachineType::F32, out.value_registers, &op.dst) ||
                !resolve_register_value(instruction.src0, MachineType::F32, out.value_registers, &op.src1) ||
                !resolve_register_value(instruction.src1, MachineType::F32, out.value_registers, &op.src2)) {
                out.error = "machine vector operation requires register-backed F32 values";
                return false;
            }
            op.op = static_cast<usse::VectorOp>(instruction.subop());
            op.dest_mask = static_cast<uint8_t>(config & 0x0f);
            op.src1_negative = (config & 0x0040u) != 0;
            op.src1_absolute = (config & 0x0080u) != 0;
            op.src2_absolute = (config & 0x0100u) != 0;
            op.skip_invalid = (config & 0x0200u) != 0;
            op.no_schedule = (config & 0x0400u) != 0;
            if (swizzle == MachineVectorSwizzle::PositionXY11) {
                op.src1_swizzle = {{usse::SwizzleChannel::X, usse::SwizzleChannel::Y,
                                    usse::SwizzleChannel::One, usse::SwizzleChannel::One}};
                op.src2_swizzle = {{usse::SwizzleChannel::Y, usse::SwizzleChannel::Y,
                                    usse::SwizzleChannel::Y, usse::SwizzleChannel::Y}};
            } else if (swizzle == MachineVectorSwizzle::Source2YYYY) {
                op.src2_swizzle = {{usse::SwizzleChannel::Y, usse::SwizzleChannel::Y,
                                    usse::SwizzleChannel::Y, usse::SwizzleChannel::Y}};
            }
            auto apply_scalar_component=[](const MachineOperand &operand, usse::Swizzle4 &channels) {
                const uint8_t component=operand.physical_component();
                if (component==0xff) return;
                const auto lane=static_cast<usse::SwizzleChannel>(component);
                channels={{lane,lane,lane,lane}};
            };
            apply_scalar_component(instruction.src0,op.src1_swizzle);
            apply_scalar_component(instruction.src1,op.src2_swizzle);
            if (!builder.instruction(op)) { out.error = "failed to encode machine V32NMAD"; return false; }
            break;
        }
        case MachineOpcode::DivF32: {
            const uint8_t components=instruction.subop();
            if (components<1 || components>4 || guard!=usse::Predicate::Always) {
                out.error="F32 division pseudo-op requires width 1..4 and no guard";
                return false;
            }
            usse::RegisterRef dst{},numerator{},denominator{};
            if (!resolve_register_value(instruction.dst,MachineType::F16,out.value_registers,&dst) ||
                !resolve_register_value(instruction.src0,MachineType::F32,out.value_registers,&numerator) ||
                !resolve_register_value(instruction.src1,MachineType::F32,out.value_registers,&denominator) ||
                dst.bank!=usse::RegisterBank::PrimaryAttribute || dst.num!=0 ||
                numerator.bank!=usse::RegisterBank::PrimaryAttribute || denominator.bank!=usse::RegisterBank::PrimaryAttribute) {
                out.error="F32 division requires fragment output0 and direct PA inputs";
                return false;
            }
            const uint8_t expected_rhs=components<=2 ? (components==1 ? 0 : 1) : 2;
            if (numerator.num!=0 || denominator.num!=expected_rhs) {
                out.error="F32 division currently covers direct Location0 / Location1 inputs only";
                return false;
            }
            if (components==1) {
                const uint8_t numerator_component=instruction.src0.physical_component();
                const uint8_t denominator_component=instruction.src1.physical_component();
                if (numerator_component!=0 || denominator_component!=1) {
                    out.error="scalar F32 division requires PA0.x / PA0.y packed inputs";
                    return false;
                }
                usse::VcompRcpScalarF32Semantic reciprocal{denominator,denominator_component};
                if (!builder.instruction(reciprocal)) {
                    out.error="failed to encode scalar F32 division reciprocal VCOMP";
                    return false;
                }
            } else {
                for (uint8_t lane=0;lane<components;++lane) {
                    usse::VcompRcpF32Semantic reciprocal{denominator,lane};
                    if (!builder.instruction(reciprocal)) {
                        out.error="failed to encode F32 division reciprocal VCOMP";
                        return false;
                    }
                }
            }
            bool staged=false;
            if (components<=2) {
                usse::VmovSemantic stage{};
                stage.dst={usse::RegisterBank::Temp,61};
                stage.src=numerator;
                stage.data_type=usse::DataType::F32;
                stage.dest_mask=components==1 ? 0x1 : 0x3;
                stage.swizzle=components==1 ? instruction.src0.physical_component() : 4;
                stage.skip_invalid=true;
                stage.no_schedule=false;
                staged=builder.instruction(stage);
            } else {
                usse::VpckSemantic stage{};
                stage.dst={usse::RegisterBank::Temp,125};
                stage.src1=numerator;
                stage.src2={numerator.bank,static_cast<uint8_t>(numerator.num+1)};
                stage.src_format=usse::PackFormat::F32;
                stage.dst_format=usse::PackFormat::F32;
                stage.dest_mask=components==3 ? 0x7 : 0xF;
                if (components==3) {
                    stage.components[0]=0; stage.components[1]=1;
                    stage.components[2]=2; stage.components[3]=0;
                }
                stage.skip_invalid=true;
                stage.no_schedule=false;
                staged=builder.instruction(stage);
            }
            usse::V16NmadDivF32Semantic combine{components};
            if (!staged || !builder.instruction(combine)) {
                out.error="failed to encode F32 division stage/combine";
                return false;
            }
            break;
        }
        case MachineOpcode::F32ToS32Color: {
            usse::RegisterRef dst{},src{};
            if (guard!=usse::Predicate::Always || instruction.subop()!=0 ||
                !resolve_register_value(instruction.dst,MachineType::S32,out.value_registers,&dst) ||
                !resolve_register_value(instruction.src0,MachineType::F32,out.value_registers,&src) ||
                dst.bank!=usse::RegisterBank::PrimaryAttribute || dst.num!=0 ||
                src.bank!=usse::RegisterBank::PrimaryAttribute || src.num!=0 ||
                (instruction.src0.physical_component()!=0xff && instruction.src0.physical_component()!=0)) {
                out.error="F32->S32 COLOR profile requires PA0.x input and PA0 output";
                return false;
            }
            usse::VpckSemantic stage{};
            stage.dst={usse::RegisterBank::PrimaryAttribute,0};
            stage.src1={usse::RegisterBank::PrimaryAttribute,0};
            stage.src2={usse::RegisterBank::Immediate,0};
            stage.src_format=usse::PackFormat::F32;
            stage.dst_format=usse::PackFormat::F16;
            stage.dest_mask=1;
            stage.no_schedule=false;
            if (!builder.instruction(stage) ||
                !builder.instruction(usse::V16NmadF32ToS32Semantic{0}) ||
                !builder.instruction(usse::V16NmadF32ToS32Semantic{1})) {
                out.error="failed to encode oracle F32->S32 COLOR sequence";
                return false;
            }
            break;
        }
        case MachineOpcode::DotSplatF32: {
            const uint8_t components=instruction.subop();
            if ((components!=2 && components!=3) || guard!=usse::Predicate::Always) {
                out.error="narrow F32 dot-splat pseudo-op requires width 2/3 and no guard";
                return false;
            }
            usse::RegisterRef dst{},lhs{},rhs{};
            if (!resolve_register_value(instruction.dst,MachineType::F16,out.value_registers,&dst) ||
                !resolve_register_value(instruction.src0,MachineType::F32,out.value_registers,&lhs) ||
                !resolve_register_value(instruction.src1,MachineType::F32,out.value_registers,&rhs) ||
                dst.bank!=usse::RegisterBank::PrimaryAttribute || dst.num!=0 ||
                lhs.bank!=usse::RegisterBank::PrimaryAttribute || rhs.bank!=usse::RegisterBank::PrimaryAttribute ||
                lhs.num!=0 || rhs.num!=(components==2 ? 1 : 2)) {
                out.error="narrow F32 dot-splat currently covers direct Location0 / Location1 inputs only";
                return false;
            }
            bool staged=false;
            if (components==2) {
                usse::V32NmadSemantic mul{};
                mul.dst={usse::RegisterBank::Temp,60};
                mul.src1=rhs;
                mul.src2={usse::RegisterBank::Special,1};
                mul.op=usse::VectorOp::Mul;
                mul.dest_mask=0xF;
                mul.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                                   usse::SwizzleChannel::Zero,usse::SwizzleChannel::Zero}};
                mul.src2_swizzle={{usse::SwizzleChannel::Y,usse::SwizzleChannel::Y,
                                   usse::SwizzleChannel::Y,usse::SwizzleChannel::Y}};
                mul.skip_invalid=true;
                mul.no_schedule=true;
                usse::VmovSemantic move{};
                move.dst={usse::RegisterBank::Temp,61};
                move.src=lhs;
                move.data_type=usse::DataType::F32;
                move.dest_mask=0x3;
                move.swizzle=4;
                move.skip_invalid=true;
                move.no_schedule=false;
                staged=builder.instruction(mul) && builder.instruction(move);
            } else {
                usse::VpckSemantic a{};
                a.dst={usse::RegisterBank::Temp,124};
                a.src1=lhs; a.src2={lhs.bank,static_cast<uint8_t>(lhs.num+1)};
                a.src_format=usse::PackFormat::F32; a.dst_format=usse::PackFormat::F32;
                a.dest_mask=0x7; a.components[3]=0; a.skip_invalid=true; a.no_schedule=true;
                usse::VpckSemantic b=a;
                b.dst={usse::RegisterBank::Temp,125};
                b.src1=rhs; b.src2={rhs.bank,static_cast<uint8_t>(rhs.num+1)};
                b.no_schedule=false;
                staged=builder.instruction(a) && builder.instruction(b);
            }
            usse::V16NmadDotSplatF32Semantic combine{components};
            if (!staged || !builder.instruction(combine)) {
                out.error="failed to encode narrow F32 dot-splat stage/reduction";
                return false;
            }
            break;
        }
        case MachineOpcode::Vmad: {
            if (instruction.subop() > 3) { out.error = "invalid machine VMAD lane profile"; return false; }
            const uint16_t config = instruction.config();
            if (config & ~0x001fu) { out.error = "invalid machine VMAD config"; return false; }
            usse::VmadSemantic mad{};
            usse::RegisterRef gpi0_reg{}, gpi1_reg{};
            if (!resolve_register_value(instruction.dst, MachineType::F32, out.value_registers, &mad.dst) ||
                !resolve_register_value(instruction.src0, MachineType::F32, out.value_registers, &mad.src1) ||
                !resolve_virtual_pair(instruction.src1, MachineType::F32, out.value_registers,
                                      &gpi0_reg, &gpi1_reg)) {
                out.error = "machine VMAD requires F32 destination/source and a virtual GPI pair";
                return false;
            }
            mad.gpi0 = machine_gpi_index(gpi0_reg);
            mad.gpi1 = machine_gpi_index(gpi1_reg);
            if (mad.gpi0 == 0xff || mad.gpi1 == 0xff) {
                out.error = "machine VMAD pair was not allocated to GPI aliases";
                return false;
            }
            mad.vec4 = true;
            mad.repeat_mode = usse::RepeatMode::Slmsi;
            mad.skip_invalid = true;
            mad.write_mask = static_cast<uint8_t>(config & 0x0f);
            mad.no_schedule = (config & 0x0010u) != 0;
            const usse::SwizzleChannel lanes[] = {
                usse::SwizzleChannel::X, usse::SwizzleChannel::Y,
                usse::SwizzleChannel::Z, usse::SwizzleChannel::Z,
            };
            mad.gpi0_swizzle = {{lanes[instruction.subop()], lanes[instruction.subop()],
                                 lanes[instruction.subop()], lanes[instruction.subop()]}};
            if (instruction.subop() == 3) {
                mad.gpi1_swizzle = {{usse::SwizzleChannel::Z, usse::SwizzleChannel::W,
                                     usse::SwizzleChannel::Z, usse::SwizzleChannel::W}};
            }
            if (!builder.instruction(mad)) { out.error = "failed to encode machine VMAD"; return false; }
            break;
        }
        case MachineOpcode::VmadUniformMat4: {
            usse::VmadSemantic mad{};
            usse::RegisterRef gpi{};
            if (instruction.subop()!=0 || guard!=usse::Predicate::Always ||
                !resolve_register_value(instruction.dst,MachineType::F32,out.value_registers,&mad.dst) ||
                !resolve_register_value(instruction.src0,MachineType::F32,out.value_registers,&gpi) ||
                machine_gpi_index(gpi)!=0) {
                out.error="uniform mat4 VMAD requires F32 output and oracle GPI0 staging";
                return false;
            }
            mad.src1={usse::RegisterBank::SecondaryAttribute,0};
            mad.gpi0=0;
            mad.gpi1=2;
            mad.write_mask=1;
            mad.gpi0_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                               usse::SwizzleChannel::Z,usse::SwizzleChannel::W}};
            mad.src1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                               usse::SwizzleChannel::X,usse::SwizzleChannel::Y}};
            mad.gpi1_swizzle={{usse::SwizzleChannel::X,usse::SwizzleChannel::Y,
                               usse::SwizzleChannel::Z,usse::SwizzleChannel::Z}};
            mad.vec4=true;
            mad.control_bit_53=false;
            mad.repeat_mode=usse::RepeatMode::External;
            mad.repeat_count=3;
            mad.skip_invalid=true;
            mad.no_schedule=false;
            if (!builder.instruction(mad)) {
                out.error="failed to encode oracle uniform-mat4 VMAD";
                return false;
            }
            break;
        }
        case MachineOpcode::DependentSample: {
            if (instruction.subop() != 0 || guard != usse::Predicate::Always) {
                out.error = "dependent sample pseudo-op currently supports only sampler 0";
                return false;
            }
            usse::RegisterRef dst{}, coord{};
            if (!resolve_register_value(instruction.dst, MachineType::F32, out.value_registers, &dst) ||
                !resolve_register_value(instruction.src0, MachineType::F32, out.value_registers, &coord) ||
                dst.bank != usse::RegisterBank::Temp || machine_gpi_index(coord) == 0xff) {
                out.error = "dependent sample pseudo-op requires a TEMP F32 result and GPI coordinate";
                return false;
            }
            break;
        }
        case MachineOpcode::Branch: {
            if (instruction.src0.kind() != MachineOperandKind::Label ||
                instruction.src0.id() >= program.labels().size()) {
                out.error = "machine branch target is invalid";
                return false;
            }
            const uint32_t target_machine = program.labels()[instruction.src0.id()];
            const int64_t offset = static_cast<int64_t>(word_positions[target_machine]) -
                static_cast<int64_t>(word_positions[instruction_index]);
            if (offset < -(1 << 19) || offset >= (1 << 19)) {
                out.error = "machine branch offset exceeds signed-20 USSE range";
                return false;
            }
            usse::BranchSemantic branch{};
            branch.predicate = guard;
            branch.offset = static_cast<int32_t>(offset);
            if (!builder.instruction(branch)) {
                out.error = "failed to encode machine branch";
                return false;
            }
            break;
        }
        case MachineOpcode::LoopCounterInit: {
            usse::RegisterRef state{};
            if (instruction.subop()!=0 || guard!=usse::Predicate::Always ||
                !resolve_register_value(instruction.dst,MachineType::S32,out.value_registers,&state) ||
                state.bank!=usse::RegisterBank::Temp || state.num!=0) {
                out.error="loop counter init requires the oracle-validated S32 TEMP0 state";
                return false;
            }
            usse::VbwSemantic init{};
            init.op=usse::BitwiseOp::Or;
            init.dst=state;
            init.src1={usse::RegisterBank::SecondaryAttribute,2};
            init.src2_is_immediate=true;
            init.immediate=0;
            if (!builder.instruction(init)) {
                out.error="failed to encode oracle loop-counter VBW init";
                return false;
            }
            break;
        }
        case MachineOpcode::LoopIncrement: {
            usse::RegisterRef state{};
            const uint8_t step=instruction.subop();
            if (step<1 || step>3 || guard!=usse::Predicate::Always ||
                !resolve_register_value(instruction.dst,MachineType::S32,out.value_registers,&state) ||
                state.bank!=usse::RegisterBank::Temp || state.num!=0) {
                out.error="loop increment requires S32 TEMP0 and oracle-validated step 1..3";
                return false;
            }
            usse::I32Mad2Semantic update{};
            update.dst={usse::RegisterBank::Temp,1};
            update.src0={usse::RegisterBank::SecondaryAttribute,3};
            update.src1=state;
            update.src2={usse::RegisterBank::Immediate,step};
            update.sn=0;
            usse::I32Mad2Semantic feed{};
            feed.dst=state;
            feed.src0={usse::RegisterBank::SecondaryAttribute,3};
            feed.src1=state;
            feed.src2={usse::RegisterBank::Temp,1};
            feed.sn=1;
            if (!builder.instruction(update) || !builder.instruction(feed)) {
                out.error="failed to encode oracle loop I32MAD2 update pair";
                return false;
            }
            break;
        }
        case MachineOpcode::Compare: {
            if (instruction.subop() > static_cast<uint8_t>(usse::CompareOp::GreaterEqual)) {
                out.error = "invalid compare subop";
                return false;
            }
            if (instruction.dst.id() >= out.predicate_registers.size() ||
                out.predicate_registers[instruction.dst.id()] == 0xff) {
                out.error = "compare predicate was not allocated";
                return false;
            }
            if (instruction.src0.type() != instruction.src1.type()) {
                out.error = "machine compare source types do not match";
                return false;
            }
            if (instruction.src0.type() == MachineType::U32) {
                usse::VtstSemantic compare{};
                if (!resolve_register_value(instruction.src0, MachineType::U32, out.value_registers, &compare.lhs) ||
                    !resolve_register_value(instruction.src1, MachineType::U32, out.value_registers, &compare.rhs)) {
                    out.error = "machine compare requires register-backed U32 sources";
                    return false;
                }
                compare.predicate = guard;
                compare.op = static_cast<usse::CompareOp>(instruction.subop());
                compare.predicate_destination = out.predicate_registers[instruction.dst.id()];
                if (!builder.instruction(compare)) { out.error = "failed to encode machine U32 compare"; return false; }
            } else if (instruction.src0.type() == MachineType::F32) {
                usse::VtstF32Semantic compare{};
                if (!resolve_register_value(instruction.src0, MachineType::F32, out.value_registers, &compare.lhs) ||
                    !resolve_register_value(instruction.src1, MachineType::F32, out.value_registers, &compare.rhs)) {
                    out.error = "machine compare requires register-backed F32 sources";
                    return false;
                }
                compare.predicate = guard;
                compare.op = static_cast<usse::CompareOp>(instruction.subop());
                compare.predicate_destination = out.predicate_registers[instruction.dst.id()];
                if (!builder.instruction(compare)) { out.error = "failed to encode machine F32 compare"; return false; }
            } else if (instruction.src0.type() == MachineType::S32) {
                usse::VtstS32Semantic compare{};
                if (!resolve_register_value(instruction.src0,MachineType::S32,out.value_registers,&compare.lhs) ||
                    !resolve_register_value(instruction.src1,MachineType::S32,out.value_registers,&compare.rhs)) {
                    out.error="machine compare requires register-backed S32 sources";
                    return false;
                }
                compare.predicate=guard;
                compare.op=static_cast<usse::CompareOp>(instruction.subop());
                compare.predicate_destination=out.predicate_registers[instruction.dst.id()];
                if (!builder.instruction(compare)) { out.error="failed to encode machine S32 compare"; return false; }
            } else {
                out.error = "machine compare type is outside the validated U32/F32/S32 subset";
                return false;
            }
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
            usse::KillSemantic kill{};
            kill.predicate = predicate;
            if (!builder.instruction(kill)) { out.error = "failed to encode machine kill"; return false; }
            break;
        }
        case MachineOpcode::Bitwise: {
            if (instruction.subop() > static_cast<uint8_t>(usse::BitwiseOp::ArithmeticShiftRight)) {
                out.error = "invalid bitwise subop";
                return false;
            }
            const MachineType type=instruction.dst.type();
            if (type!=MachineType::U32 && type!=MachineType::S32) {
                out.error = "bitwise destination must be U32 or S32";
                return false;
            }
            usse::VbwSemantic op{};
            op.op = static_cast<usse::BitwiseOp>(instruction.subop());
            if (!resolve_register_value(instruction.dst,type,out.value_registers,&op.dst)) {
                out.error="bitwise destination is not register-backed";
                return false;
            }
            op.predicate = guard;
            if (!resolve_register_value(instruction.src0,type,out.value_registers,&op.src1)) {
                out.error = "bitwise source1 must be a matching register-backed integer value";
                return false;
            }
            if (instruction.src1.kind() == MachineOperandKind::Literal) {
                if (instruction.src1.type() != type || instruction.src1.id() >= program.literals().size()) {
                    out.error = "bitwise literal source is invalid";
                    return false;
                }
                op.src2_is_immediate = true;
                op.immediate = program.literals()[instruction.src1.id()];
            } else if (!resolve_register_value(instruction.src1,type,out.value_registers,&op.src2)) {
                out.error = "bitwise source2 must be a matching integer register or literal";
                return false;
            }
            if (!builder.instruction(op)) { out.error = "failed to encode machine bitwise operation"; return false; }
            break;
        }
        case MachineOpcode::Count:
            out.error = "invalid machine opcode sentinel";
            return false;
        }
    }
    out.words = builder.words();
    return true;
}

} // namespace vsc::backend
