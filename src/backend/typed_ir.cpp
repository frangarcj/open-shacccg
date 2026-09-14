#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <limits>
#include <unordered_map>

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
    case TypedType::F32x4: return MachineType::F32;
    case TypedType::F32x2:
    case TypedType::F32x3:
    case TypedType::F16x2:
    case TypedType::F16x3:
    case TypedType::F16x4:
    case TypedType::U32x2:
    case TypedType::U32x3:
    case TypedType::U32x4:
    case TypedType::Sampler2D:
    case TypedType::Invalid: return MachineType::Invalid;
    }
    return MachineType::Invalid;
}

TypedSemantic infer_semantic(const std::string &name) {
    std::string lower;
    lower.reserve(name.size());
    for (char c : name) lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (lower.find("position") != std::string::npos || lower == "pos") return TypedSemantic::Position;
    if (lower.find("color") != std::string::npos || lower.find("colour") != std::string::npos) return TypedSemantic::Color;
    if (lower.find("tex") != std::string::npos || lower.find("uv") != std::string::npos) return TypedSemantic::TexCoord;
    return TypedSemantic::None;
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

uint8_t typed_component_count(TypedType type) {
    switch (type) {
    case TypedType::F32:
    case TypedType::F16:
    case TypedType::U32:
    case TypedType::U16:
    case TypedType::S32:
        return 1;
    case TypedType::F32x2:
    case TypedType::F16x2:
    case TypedType::U32x2:
        return 2;
    case TypedType::F32x3:
    case TypedType::F16x3:
    case TypedType::U32x3:
        return 3;
    case TypedType::F32x4:
    case TypedType::F16x4:
    case TypedType::U32x4:
        return 4;
    case TypedType::Sampler2D:
    case TypedType::Invalid:
        return 0;
    }
    return 0;
}

bool typed_is_float(TypedType type) {
    switch (type) {
    case TypedType::F32:
    case TypedType::F16:
    case TypedType::F32x2:
    case TypedType::F32x3:
    case TypedType::F32x4:
    case TypedType::F16x2:
    case TypedType::F16x3:
    case TypedType::F16x4:
        return true;
    default:
        return false;
    }
}

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

TypedValue TypedProgram::input(TypedType type, uint16_t location) {
    auto dst = make_value(type);
    if (dst.kind() == TypedValueKind::None || !emit<TypedOpcode::Input>(0, dst, {}, {}, location)) return {};
    return dst;
}

TypedValue TypedProgram::uniform(TypedType type, uint16_t resource_index) {
    auto dst = make_value(type);
    if (dst.kind() == TypedValueKind::None || !emit<TypedOpcode::Uniform>(0, dst, {}, {}, resource_index)) return {};
    return dst;
}

TypedValue TypedProgram::sampler(uint16_t binding) {
    auto dst = make_value(TypedType::Sampler2D);
    if (dst.kind() == TypedValueKind::None || !emit<TypedOpcode::Sampler>(0, dst, {}, {}, binding)) return {};
    return dst;
}

uint16_t TypedProgram::make_label() {
    if (labels_.size() >= std::numeric_limits<uint16_t>::max())
        return std::numeric_limits<uint16_t>::max();
    const uint16_t id = static_cast<uint16_t>(labels_.size());
    labels_.push_back(std::numeric_limits<uint32_t>::max());
    return id;
}

bool TypedProgram::bind_label(uint16_t label) {
    if (label >= labels_.size() || labels_[label] != std::numeric_limits<uint32_t>::max()) return false;
    labels_[label] = static_cast<uint32_t>(instructions_.size());
    return true;
}

bool TypedProgram::jump(uint16_t label) {
    return label < labels_.size() && emit<TypedOpcode::Jump>(0, {}, {}, {}, label);
}

bool TypedProgram::branch(uint16_t label, TypedValue predicate) {
    return label < labels_.size() && predicate.kind() == TypedValueKind::Predicate &&
        emit<TypedOpcode::Branch>(0, {}, predicate, {}, label);
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

uint16_t TypedShader::add_resource(TypedResourceKind kind, TypedValue value, TypedType type,
                                   const std::string &name, uint16_t index,
                                   TypedSemantic semantic, uint8_t semantic_index) {
    if (resources_.size() >= std::numeric_limits<uint16_t>::max() ||
        names_.size() >= std::numeric_limits<uint16_t>::max())
        return std::numeric_limits<uint16_t>::max();
    const uint16_t name_index = static_cast<uint16_t>(names_.size());
    names_.push_back(name);
    resources_.push_back({value, name_index, index, kind, type, semantic, semantic_index});
    return static_cast<uint16_t>(resources_.size() - 1);
}

const std::string &TypedShader::resource_name(const TypedResource &resource) const {
    static const std::string empty;
    return resource.name_index < names_.size() ? names_[resource.name_index] : empty;
}

static bool lower_typed_program_impl(const TypedProgram &typed, MachineProgram &machine, std::string &error,
                                     bool reset_machine, MachineOperand *stored_output = nullptr,
                                     TypedType *stored_type = nullptr, uint16_t *stored_resource = nullptr,
                                     bool emit_fragment_stores = false,
                                     uint16_t fragment_output_resource = std::numeric_limits<uint16_t>::max()) {
    if (reset_machine) machine = {};
    error.clear();
    if (stored_output) *stored_output = {};
    if (stored_type) *stored_type = TypedType::Invalid;
    if (stored_resource) *stored_resource = 0;
    std::vector<MachineOperand> values(typed.value_count());
    std::vector<MachineOperand> predicates(typed.predicate_count());
    std::vector<MachineOperand> literals(typed.literals().size());
    std::vector<TypedType> value_types(typed.value_count(), TypedType::Invalid);
    std::vector<bool> value_defined(typed.value_count(), false);
    std::vector<bool> predicate_defined(typed.predicate_count(), false);

    std::vector<MachineOperand> machine_labels;
    machine_labels.reserve(typed.labels().size());
    for (uint32_t position : typed.labels()) {
        if (position == std::numeric_limits<uint32_t>::max() || position > typed.instructions().size()) {
            error = "typed control-flow label is unbound or out of range";
            return false;
        }
        const auto label = machine.make_label();
        if (label.kind() == MachineOperandKind::None) {
            error = "failed to allocate machine control-flow label";
            return false;
        }
        machine_labels.push_back(label);
    }

    auto bind_labels_at = [&](uint32_t position) -> bool {
        for (uint32_t id=0; id<typed.labels().size(); ++id) {
            if (typed.labels()[id] == position && !machine.bind_label(machine_labels[id])) {
                error = "failed to bind machine control-flow label";
                return false;
            }
        }
        return true;
    };

    for (uint32_t instruction_index=0; instruction_index<typed.instructions().size(); ++instruction_index) {
        if (!bind_labels_at(instruction_index)) return false;
        const auto &instruction = typed.instructions()[instruction_index];
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
            uint16_t physical_index = instruction.aux;
            if (instruction.opcode() == TypedOpcode::Input && instruction.dst.type() == TypedType::F32x4) {
                physical_index = static_cast<uint16_t>(instruction.aux * 2u);
            }
            if (instruction.opcode() == TypedOpcode::Uniform && instruction.dst.type() == TypedType::F32x4) {
                if ((instruction.aux & 3u) != 0) { error = "float4 uniform word offset is not vec4 aligned"; return false; }
                physical_index = instruction.aux / 2u;
            }
            if (physical_index >= 128) { error = "resource physical index exceeds current register subset"; return false; }
            values[instruction.dst.id()] = machine.physical(bank, static_cast<uint8_t>(physical_index), type);
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::Sampler:
        case TypedOpcode::ConstructPosition:
        case TypedOpcode::TransformPosition:
        case TypedOpcode::Sample2D:
            error = "high-level typed shader operation requires compile_typed_shader";
            return false;
        case TypedOpcode::StoreOutput: {
            if (emit_fragment_stores) {
                if (instruction.aux != fragment_output_resource || instruction.src0.type() != TypedType::F32x4) {
                    error = "direct fragment StoreOutput is outside the validated float4 Location 0 subset";
                    return false;
                }
                const auto value = lower_value(typed,instruction.src0,values,literals,machine);
                if (value.kind()==MachineOperandKind::None ||
                    !machine.emit_config<MachineOpcode::Pack>(
                        machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                        machine_pack_config(0xF,true,false),
                        machine.physical(machine_fragment_output(0),MachineType::F16),value,
                        machine.physical(machine_immediate(0),MachineType::F32))) {
                    error = "failed to lower branch-local fragment output pack";
                    return false;
                }
                break;
            }
            if (!stored_output || stored_output->kind() != MachineOperandKind::None) {
                error = stored_output ? "typed fragment has multiple output stores" :
                    "high-level typed shader operation requires compile_typed_shader";
                return false;
            }
            const auto value = lower_value(typed, instruction.src0, values, literals, machine);
            if (value.kind() == MachineOperandKind::None) { error = "typed output value did not lower to Machine IR"; return false; }
            *stored_output = value;
            if (stored_type) *stored_type = instruction.src0.type();
            if (stored_resource) *stored_resource = instruction.aux;
            break;
        }
        case TypedOpcode::Jump:
            if (instruction.aux >= machine_labels.size() || !machine.branch(machine_labels[instruction.aux])) {
                error = "failed to lower typed jump to Machine IR";
                return false;
            }
            break;
        case TypedOpcode::Branch: {
            if (instruction.aux >= machine_labels.size() || instruction.src0.id() >= predicates.size()) {
                error = "typed branch target or predicate is invalid";
                return false;
            }
            auto predicate = predicates[instruction.src0.id()];
            if (predicate.kind() == MachineOperandKind::None) {
                error = "typed branch predicate was not lowered";
                return false;
            }
            if (instruction.src0.inverted())
                predicate = MachineOperand::virtual_predicate(predicate.id(), !predicate.inverted());
            if (!machine.branch(machine_labels[instruction.aux], predicate)) {
                error = "failed to lower typed conditional branch to Machine IR";
                return false;
            }
            break;
        }
        case TypedOpcode::FloatBinary: {
            const auto float_op = static_cast<TypedFloatOp>(instruction.subop());
            const bool vector4 = instruction.dst.type() == TypedType::F32x4 &&
                instruction.src0.type() == TypedType::F32x4 && instruction.src1.type() == TypedType::F32x4;
            const bool dot = float_op == TypedFloatOp::Dot && instruction.dst.type() == TypedType::F32 &&
                instruction.src0.type() == TypedType::F32x4 && instruction.src1.type() == TypedType::F32x4;
            if ((!vector4 && !dot) || float_op > TypedFloatOp::Dot) {
                error = "typed float binary currently supports F32x4 arithmetic and float4 dot";
                return false;
            }

            auto src0 = lower_value(typed, instruction.src0, values, literals, machine);
            auto src1 = lower_value(typed, instruction.src1, values, literals, machine);
            const auto dst = machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
            if (dst.kind() == MachineOperandKind::None || src0.kind() == MachineOperandKind::None ||
                src1.kind() == MachineOperandKind::None) {
                error = "failed to create machine operands for typed float operation";
                return false;
            }

            usse::VectorOp machine_op;
            bool src0_negative = false;
            switch (float_op) {
            case TypedFloatOp::Mul: machine_op = usse::VectorOp::Mul; break;
            case TypedFloatOp::Add: machine_op = usse::VectorOp::Add; break;
            case TypedFloatOp::Sub:
                machine_op = usse::VectorOp::Add;
                std::swap(src0, src1);
                src0_negative = true;
                break;
            case TypedFloatOp::Min: machine_op = usse::VectorOp::Min; break;
            case TypedFloatOp::Max: machine_op = usse::VectorOp::Max; break;
            case TypedFloatOp::Dot: machine_op = usse::VectorOp::Dot; break;
            }
            if (!machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(machine_op),
                    machine_vector_config(dot ? 0x1 : 0xF, MachineVectorSwizzle::Identity, src0_negative),
                    dst, src0, src1)) {
                error = "failed to lower typed float operation to Machine IR";
                return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::FloatUnary: {
            const auto unary_op = static_cast<TypedFloatUnaryOp>(instruction.subop());
            if (instruction.dst.type() != TypedType::F32x4 || instruction.src0.type() != TypedType::F32x4 ||
                unary_op > TypedFloatUnaryOp::Abs) {
                error = "typed float unary currently supports F32x4 negate/absolute";
                return false;
            }
            const auto src = lower_value(typed, instruction.src0, values, literals, machine);
            const auto dst = machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
            const auto zero = machine.physical(machine_immediate(0), MachineType::F32);
            if (dst.kind() == MachineOperandKind::None || src.kind() == MachineOperandKind::None ||
                !machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Add),
                    machine_vector_config(0xF, MachineVectorSwizzle::Identity,
                        unary_op == TypedFloatUnaryOp::Neg, unary_op == TypedFloatUnaryOp::Abs),
                    dst, src, zero)) {
                error = "failed to lower typed float unary operation to Machine IR";
                return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::FloatExtractX: {
            const auto components=typed_component_count(instruction.src0.type());
            if (instruction.dst.type()!=TypedType::F32 || !typed_is_float(instruction.src0.type()) ||
                components<2 || components>4) {
                error = "typed float extract-X requires a float vector source and F32 destination";
                return false;
            }
            const auto src=lower_value(typed,instruction.src0,values,literals,machine);
            if (src.kind()==MachineOperandKind::None) {
                error = "failed to lower float extract-X source";
                return false;
            }
            // X aliases the base F32 register directly; no USSE instruction is needed.
            values[instruction.dst.id()]=src;
            value_types[instruction.dst.id()]=TypedType::F32;
            value_defined[instruction.dst.id()]=true;
            break;
        }
        case TypedOpcode::FloatSplat: {
            if (instruction.dst.type() != TypedType::F32x4 ||
                (instruction.src0.type() != TypedType::F32 && instruction.src0.type() != TypedType::F32x4)) {
                error = "typed float splat currently supports F32/F32x4 to F32x4";
                return false;
            }
            const auto src = lower_value(typed, instruction.src0, values, literals, machine);
            const auto dst = machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
            if (src.kind() == MachineOperandKind::None || dst.kind() == MachineOperandKind::None ||
                !machine.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                    machine_move_config(0xF, 0, 0, true, false), dst, src)) {
                error = "failed to lower typed float splat to Machine IR";
                return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = TypedType::F32x4;
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::FloatConvert: {
            const auto convert = static_cast<TypedFloatConvertOp>(instruction.subop());
            if (convert != TypedFloatConvertOp::F32x4ToF16x4 ||
                instruction.dst.type() != TypedType::F16x4 || instruction.src0.type() != TypedType::F32x4) {
                error = "typed float convert currently supports only F32x4 to F16x4";
                return false;
            }
            const auto src = lower_value(typed, instruction.src0, values, literals, machine);
            const auto dst = machine.make_value<MachineType::F16>(MachineRegisterClass::FloatTemp, 1);
            if (src.kind() == MachineOperandKind::None || dst.kind() == MachineOperandKind::None ||
                !machine.emit_config<MachineOpcode::PackValue>(
                    machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F16),
                    machine_pack_config(0xF, true, false), dst, src)) {
                error = "failed to lower typed float conversion to Machine IR";
                return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = TypedType::F16x4;
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
            const bool u32 = instruction.src0.type() == TypedType::U32 && instruction.src1.type() == TypedType::U32;
            const bool f32 = instruction.src0.type() == TypedType::F32 && instruction.src1.type() == TypedType::F32;
            if ((!u32 && !f32) ||
                instruction.subop() > static_cast<uint8_t>(usse::CompareOp::GreaterEqual)) {
                error = "typed compare currently requires matching U32 or F32 scalar operands"; return false;
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
    if (!bind_labels_at(static_cast<uint32_t>(typed.instructions().size()))) return false;
    return true;
}

bool lower_typed_program(const TypedProgram &typed, MachineProgram &machine, std::string &error) {
    return lower_typed_program_impl(typed, machine, error, true);
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

bool compile_typed_shader(const TypedShader &shader, IrCompileResult &out) {
    out = {};
    const auto &program = shader.program();
    const auto &instructions = program.instructions();
    const auto &resources = shader.resources();

    std::vector<const TypedInstruction *> defs(program.value_count(), nullptr);
    for (const auto &instruction : instructions) {
        if (instruction.dst.kind() == TypedValueKind::Value && instruction.dst.id() < defs.size()) {
            if (defs[instruction.dst.id()]) { out.error = "typed shader value is defined twice"; return false; }
            defs[instruction.dst.id()] = &instruction;
        }
    }

    std::vector<const TypedResource *> resource_by_value(program.value_count(), nullptr);
    for (const auto &resource : resources) {
        if (resource.value.kind() != TypedValueKind::Value) continue;
        if (resource.value.id() >= resource_by_value.size()) { out.error = "typed resource value is out of range"; return false; }
        resource_by_value[resource.value.id()] = &resource;
    }

    auto resource_for_value = [&](TypedValue value) -> const TypedResource * {
        if (value.kind() != TypedValueKind::Value || value.id() >= resource_by_value.size()) return nullptr;
        return resource_by_value[value.id()];
    };
    auto definition = [&](TypedValue value) -> const TypedInstruction * {
        if (value.kind() != TypedValueKind::Value || value.id() >= defs.size()) return nullptr;
        return defs[value.id()];
    };

    if (shader.stage() == TypedStage::Vertex) {
        std::vector<IrAttribute> vertex_attributes;
        std::vector<IrMatrix4Uniform> vertex_matrices;
        std::vector<const TypedResource *> inputs;
        std::vector<const TypedResource *> matrices;
        for (const auto &resource : resources) {
            if (resource.kind == TypedResourceKind::Input) inputs.push_back(&resource);
            else if (resource.kind == TypedResourceKind::Matrix4) matrices.push_back(&resource);
        }
        std::sort(inputs.begin(), inputs.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
        std::sort(matrices.begin(), matrices.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
        if (inputs.empty()) { out.error = "typed vertex shader has no inputs"; return false; }

        std::unordered_map<uint32_t, uint32_t> attribute_for_value;
        for (const auto *resource : inputs) {
            const uint8_t components = typed_component_count(resource->type);
            if (!typed_is_float(resource->type) || components < 2 || components > 4 ||
                resource->value.kind() != TypedValueKind::Value) {
                out.error = "typed vertex input is not a supported float vector";
                return false;
            }
            const uint32_t attribute = static_cast<uint32_t>(vertex_attributes.size());
            attribute_for_value[resource->value.id()] = attribute;
            vertex_attributes.push_back({shader.resource_name(*resource), components,
                                         static_cast<uint32_t>(resource->index) * 4u});
        }

        std::unordered_map<uint16_t, uint32_t> matrix_for_resource;
        for (const auto *resource : matrices) {
            const uint16_t resource_id = static_cast<uint16_t>(resource - resources.data());
            matrix_for_resource[resource_id] = static_cast<uint32_t>(vertex_matrices.size());
            vertex_matrices.push_back({shader.resource_name(*resource), resource->index});
        }

        bool position_written = false;
        bool constructed_position = false;
        bool transformed_position = false;
        uint32_t position_attribute = 0;
        uint32_t position_matrix = 0;
        bool varying_written = false;
        uint32_t varying_attribute = 0;
        IrVaryingSemantic selected_varying_semantic = IrVaryingSemantic::TexCoord;
        for (const auto &instruction : instructions) {
            if (instruction.opcode() != TypedOpcode::StoreOutput) continue;
            if (instruction.aux >= resources.size()) { out.error = "typed vertex output resource is out of range"; return false; }
            const auto &output_resource = resources[instruction.aux];
            if (output_resource.kind != TypedResourceKind::Output) { out.error = "typed StoreOutput does not reference an output"; return false; }

            TypedSemantic semantic = output_resource.semantic;
            if (semantic == TypedSemantic::None) semantic = infer_semantic(shader.resource_name(output_resource));
            if (semantic == TypedSemantic::Position) {
                if (position_written) { out.error = "typed vertex shader writes position twice"; return false; }
                const auto *def = definition(instruction.src0);
                if (!def) { out.error = "typed vertex position has no defining operation"; return false; }
                if (def->opcode() == TypedOpcode::ConstructPosition) {
                    const auto it = attribute_for_value.find(def->src0.id());
                    if (def->src0.kind() != TypedValueKind::Value || it == attribute_for_value.end()) {
                        out.error = "typed constructed position is not sourced by a vertex input"; return false;
                    }
                    constructed_position = true;
                    position_attribute = it->second;
                } else if (def->opcode() == TypedOpcode::TransformPosition) {
                    TypedValue source = def->src0;
                    if (const auto *construct = definition(source); construct && construct->opcode() == TypedOpcode::ConstructPosition)
                        source = construct->src0;
                    const auto attr_it = attribute_for_value.find(source.id());
                    const auto matrix_it = matrix_for_resource.find(def->aux);
                    if (source.kind() != TypedValueKind::Value || attr_it == attribute_for_value.end() || matrix_it == matrix_for_resource.end()) {
                        out.error = "typed transformed position has unresolved input or matrix"; return false;
                    }
                    transformed_position = true;
                    position_attribute = attr_it->second;
                    position_matrix = matrix_it->second;
                } else {
                    out.error = "typed vertex position producer is unsupported"; return false;
                }
                position_written = true;
                continue;
            }

            const auto source_it = attribute_for_value.find(instruction.src0.id());
            if (instruction.src0.kind() != TypedValueKind::Value || source_it == attribute_for_value.end()) {
                out.error = "typed varying output must currently copy a vertex input"; return false;
            }
            if (semantic == TypedSemantic::None) {
                const auto *input_resource = resource_for_value(instruction.src0);
                if (input_resource) semantic = infer_semantic(shader.resource_name(*input_resource));
            }
            IrVaryingSemantic varying_semantic;
            if (semantic == TypedSemantic::Color) varying_semantic = IrVaryingSemantic::Color;
            else if (semantic == TypedSemantic::TexCoord) varying_semantic = IrVaryingSemantic::TexCoord;
            else { out.error = "typed vertex varying semantic is unknown"; return false; }
            if (varying_written) { out.error = "typed vertex shader writes multiple varyings"; return false; }
            varying_written = true;
            varying_attribute = source_it->second;
            selected_varying_semantic = varying_semantic;
        }
        if (!position_written) { out.error = "typed vertex shader does not write position"; return false; }
        if (constructed_position) {
            if (transformed_position || varying_written || vertex_attributes.size()!=1 || !vertex_matrices.empty() ||
                position_attribute>=vertex_attributes.size()) {
                out.error = "typed constructed-position vertex shape is unsupported";
                return false;
            }
            return compile_vertex_construct_position(vertex_attributes[position_attribute],0,0,out);
        }
        if (!transformed_position || !varying_written || vertex_attributes.size()!=2 || vertex_matrices.size()!=1 ||
            position_attribute>=vertex_attributes.size() || varying_attribute>=vertex_attributes.size() ||
            position_attribute==varying_attribute || position_matrix!=0) {
            out.error = "typed matrix vertex shape is unsupported";
            return false;
        }
        return compile_vertex_matrix_path(vertex_attributes[position_attribute],vertex_attributes[varying_attribute],
                                          vertex_matrices[0],selected_varying_semantic,0,0,out);
    }

    std::vector<IrUniformVec4> fragment_uniforms;
    std::vector<IrSampler2D> fragment_samplers;
    std::vector<const TypedResource *> inputs;
    std::vector<const TypedResource *> uniforms;
    std::vector<const TypedResource *> samplers;
    for (const auto &resource : resources) {
        if (resource.kind == TypedResourceKind::Input) inputs.push_back(&resource);
        else if (resource.kind == TypedResourceKind::Uniform) uniforms.push_back(&resource);
        else if (resource.kind == TypedResourceKind::Sampler2D) samplers.push_back(&resource);
    }
    std::sort(inputs.begin(), inputs.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
    std::sort(uniforms.begin(), uniforms.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
    std::sort(samplers.begin(), samplers.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
    for (const auto *resource : uniforms) {
        if (resource->type != TypedType::F32x4 || resource->value.kind() != TypedValueKind::Value) {
            out.error = "typed fragment uniform is not a supported float4"; return false;
        }
        fragment_uniforms.push_back({shader.resource_name(*resource), resource->index});
    }
    for (const auto *resource : samplers)
        fragment_samplers.push_back({shader.resource_name(*resource), resource->index});

    std::vector<const TypedInstruction *> output_stores;
    for (const auto &instruction : instructions)
        if (instruction.opcode() == TypedOpcode::StoreOutput) output_stores.push_back(&instruction);

    if (!program.labels().empty()) {
        if (output_stores.empty() || !uniforms.empty() || !samplers.empty() ||
            inputs.size()<2 || inputs.size()>3) {
            out.error = "typed fragment control path requires 2-3 float4 inputs, output stores and no uniforms/samplers";
            return false;
        }
        for (size_t i=0;i<inputs.size();++i) {
            if (inputs[i]->type!=TypedType::F32x4 || inputs[i]->index!=i) {
                out.error = "typed fragment control inputs must be contiguous float4 locations";
                return false;
            }
        }
        const uint16_t output_resource=output_stores[0]->aux;
        if (output_resource>=resources.size() || resources[output_resource].kind!=TypedResourceKind::Output ||
            resources[output_resource].index!=0 || resources[output_resource].type!=TypedType::F32x4) {
            out.error = "typed fragment control output must be float4 Location 0";
            return false;
        }
        for (const auto *store : output_stores) {
            if (store->aux!=output_resource || store->src0.type()!=TypedType::F32x4) {
                out.error = "typed fragment control stores must target the same float4 output";
                return false;
            }
        }
        MachineProgram primary;
        if (!primary.emit<MachineOpcode::Phase>()) {
            out.error = "failed to start typed fragment control Machine IR";
            return false;
        }
        std::string machine_error;
        if (!lower_typed_program_impl(program,primary,machine_error,false,nullptr,nullptr,nullptr,
                                      true,output_resource)) {
            out.error=machine_error;
            return false;
        }
        return compile_fragment_control_machine(primary,static_cast<uint8_t>(inputs.size()),0,0,out);
    }

    const TypedInstruction *store = nullptr;
    for (const auto *candidate : output_stores) {
        if (store) { out.error = "typed fragment shader has multiple output stores"; return false; }
        store = candidate;
    }
    if (!store) { out.error = "typed fragment shader has no output store"; return false; }
    const TypedValue root = store->src0;

    if (const auto *resource = resource_for_value(root)) {
        if (resource->kind == TypedResourceKind::Uniform && resource->type == TypedType::F32x4) {
            return compile_fragment_machine_profile(FragmentMachineProfile::UniformColor,
                                                    fragment_uniforms,fragment_samplers,0,0,out);
        }
        if (resource->kind == TypedResourceKind::Input && resource->type == TypedType::F32x4) {
            return compile_fragment_machine_profile(FragmentMachineProfile::VaryingColor,
                                                    fragment_uniforms,fragment_samplers,0,0,out);
        }
    }

    const auto *root_def = definition(root);
    if (!root_def) { out.error = "typed fragment root has no defining operation"; return false; }
    if (root_def->opcode() == TypedOpcode::Sample2D) {
        return compile_fragment_machine_profile(FragmentMachineProfile::Texture2D,
                                                fragment_uniforms,fragment_samplers,0,0,out);
    }
    if (root_def->opcode() == TypedOpcode::FloatBinary &&
        root_def->subop() == static_cast<uint8_t>(TypedFloatOp::Mul)) {
        const TypedInstruction *sample = nullptr;
        const TypedResource *tint = nullptr;
        if (const auto *a = definition(root_def->src0); a && a->opcode() == TypedOpcode::Sample2D) sample = a;
        if (const auto *b = definition(root_def->src1); b && b->opcode() == TypedOpcode::Sample2D) sample = b;
        if (const auto *r = resource_for_value(root_def->src0); r && r->kind == TypedResourceKind::Uniform) tint = r;
        if (const auto *r = resource_for_value(root_def->src1); r && r->kind == TypedResourceKind::Uniform) tint = r;
        if (sample && tint && uniforms.size() == 1 && samplers.size() == 1) {
            return compile_fragment_machine_profile(FragmentMachineProfile::TextureTint2D,
                                                    fragment_uniforms,fragment_samplers,0,0,out);
        }
    }

    if (!samplers.empty()) { out.error = "typed generic arithmetic path does not support samplers"; return false; }
    if (inputs.size()!=1 || inputs[0]->index!=0 || inputs[0]->type!=TypedType::F32x4) {
        out.error = "typed generic arithmetic path requires float4 input Location 0";
        return false;
    }
    for (size_t i=0;i<uniforms.size();++i) {
        if (uniforms[i]->index != i*4u) {
            out.error = "typed arithmetic float4 uniforms must use contiguous word offsets";
            return false;
        }
    }
    if (store->aux >= resources.size() || resources[store->aux].kind != TypedResourceKind::Output ||
        resources[store->aux].index != 0 || resources[store->aux].type != TypedType::F32x4) {
        out.error = "typed generic arithmetic output must be float4 Location 0";
        return false;
    }

    MachineProgram primary;
    if (!primary.emit<MachineOpcode::Phase>() || !primary.emit<MachineOpcode::Nop>()) {
        out.error = "failed to start typed arithmetic Machine IR";
        return false;
    }
    MachineOperand stored_value{};
    TypedType stored_type = TypedType::Invalid;
    uint16_t stored_resource = 0;
    std::string machine_error;
    if (!lower_typed_program_impl(program,primary,machine_error,false,
                                  &stored_value,&stored_type,&stored_resource)) {
        out.error = machine_error;
        return false;
    }
    if (stored_resource != store->aux || stored_type != TypedType::F32x4 ||
        stored_value.kind() == MachineOperandKind::None) {
        out.error = "typed arithmetic Machine IR did not produce the expected float4 output";
        return false;
    }
    if (!primary.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
            machine_pack_config(0xF,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16), stored_value,
            primary.physical(machine_immediate(0),MachineType::F32))) {
        out.error = "failed to append typed arithmetic output pack";
        return false;
    }
    return compile_fragment_arithmetic_machine(primary,fragment_uniforms,0,0,out);
}

} // namespace vsc::backend
