#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <functional>
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

enum class TypedRole : uint8_t { None, ValueUse, ValueDef, ValueUpdate, PredicateUse, PredicateDef };
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
    case TypedType::F32x2:
    case TypedType::F32x3:
    case TypedType::F32x4:
        return MachineType::F32;
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
        return value.id() < program.literals().size() &&
            (value.type() == TypedType::U32 || value.type() == TypedType::S32 || value.type()==TypedType::F32);
    return value.id() < defined.size() && defined[value.id()] && types[value.id()] == value.type();
}

MachineOperand lower_value(const TypedProgram &typed, const TypedValue &value,
                           const std::vector<MachineOperand> &values,
                           std::vector<MachineOperand> &literals,
                           MachineProgram &machine) {
    if (value.kind() == TypedValueKind::Value)
        return value.id() < values.size() ? values[value.id()] : MachineOperand{};
    if (value.kind() != TypedValueKind::Literal || value.id() >= typed.literals().size()) return {};
    if (literals[value.id()].kind() == MachineOperandKind::None) {
        if (value.type()==TypedType::U32)
            literals[value.id()] = machine.literal_u32(typed.literals()[value.id()]);
        else if (value.type()==TypedType::S32)
            literals[value.id()] = machine.literal_s32(static_cast<int32_t>(typed.literals()[value.id()]));
        else if (value.type()==TypedType::F32 && typed.literals()[value.id()]==0)
            literals[value.id()] = machine.physical(machine_immediate(0),MachineType::F32);
    }
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

TypedValue TypedProgram::literal_s32(int32_t value) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(static_cast<uint32_t>(value));
    return TypedValue::literal(static_cast<uint32_t>(literals_.size() - 1), TypedType::S32);
}

TypedValue TypedProgram::literal_f32(uint32_t bits) {
    if (literals_.size() > kPayloadMask) return {};
    literals_.push_back(bits);
    return TypedValue::literal(static_cast<uint32_t>(literals_.size() - 1), TypedType::F32);
}

TypedValue TypedProgram::literal_f32x4(const std::array<uint32_t,4> &bits) {
    if (float4_literals_.size() >= std::numeric_limits<uint16_t>::max()) return {};
    const auto dst=make_value(TypedType::F32x4);
    if (dst.kind()==TypedValueKind::None) return {};
    const uint16_t index=static_cast<uint16_t>(float4_literals_.size());
    float4_literals_.push_back(bits);
    if (!emit<TypedOpcode::FloatConstant>(0,dst,{},{},index)) {
        float4_literals_.pop_back();
        return {};
    }
    return dst;
}

TypedValue TypedProgram::compose_f32x4(const std::array<TypedValue,4> &components) {
    if (float4_composites_.size() >= std::numeric_limits<uint16_t>::max()) return {};
    for (const auto component:components)
        if (!is_value(component) || component.type()!=TypedType::F32) return {};
    const auto dst=make_value(TypedType::F32x4);
    if (dst.kind()==TypedValueKind::None) return {};
    const uint16_t index=static_cast<uint16_t>(float4_composites_.size());
    float4_composites_.push_back(components);
    if (!emit<TypedOpcode::FloatCompose>(0,dst,{},{},index)) {
        float4_composites_.pop_back();
        return {};
    }
    return dst;
}

TypedValue TypedProgram::input(TypedType type, uint16_t location) {
    auto dst = make_value(type);
    if (dst.kind() == TypedValueKind::None || !emit<TypedOpcode::Input>(0, dst, {}, {}, location)) return {};
    return dst;
}

TypedValue TypedProgram::input_component_f32(uint16_t physical_index, uint8_t component) {
    if (physical_index >= 128 || component >= 4) return {};
    auto dst=make_value(TypedType::F32);
    if (dst.kind()==TypedValueKind::None ||
        !emit<TypedOpcode::Input>(static_cast<uint8_t>(component+1u),dst,{},{},physical_index))
        return {};
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
                                     uint16_t fragment_output_resource = std::numeric_limits<uint16_t>::max(),
                                     const std::vector<MachineOperand> *literal_bindings = nullptr,
                                     std::vector<MachineOperand> *lowered_values = nullptr,
                                     bool ignore_output_stores = false) {
    if (reset_machine) machine = {};
    error.clear();
    if (stored_output) *stored_output = {};
    if (stored_type) *stored_type = TypedType::Invalid;
    if (stored_resource) *stored_resource = 0;
    std::vector<MachineOperand> values(typed.value_count());
    std::vector<MachineOperand> predicates(typed.predicate_count());
    std::vector<MachineOperand> literals(typed.literals().size());
    if (literal_bindings) {
        if (literal_bindings->size()!=literals.size()) {
            error="typed literal binding table has the wrong size";
            return false;
        }
        literals=*literal_bindings;
    }
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
            } else if (role == TypedRole::ValueUpdate) {
                if (operand.kind()!=TypedValueKind::Value || operand.type()==TypedType::Invalid ||
                    operand.id()>=value_defined.size() || !value_defined[operand.id()] ||
                    value_types[operand.id()]!=operand.type()) {
                    error=std::string(desc->name)+" requires an already-defined mutable value";
                    return false;
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
            uint8_t physical_component=0xff;
            if (instruction.opcode() == TypedOpcode::Input) {
                if (instruction.dst.type()==TypedType::F32 && instruction.subop()!=0) {
                    if (instruction.subop()>4) { error="scalar F32 input component is out of range"; return false; }
                    physical_component=static_cast<uint8_t>(instruction.subop()-1u);
                } else if (instruction.subop()!=0) {
                    error="input component selector is only validated for scalar F32";
                    return false;
                } else if (instruction.dst.type()==TypedType::F32x2)
                    physical_index=instruction.aux;
                else if (instruction.dst.type()==TypedType::F32x3 || instruction.dst.type()==TypedType::F32x4)
                    physical_index = static_cast<uint16_t>(instruction.aux * 2u);
            }
            if (instruction.opcode() == TypedOpcode::Uniform && typed_is_float(instruction.dst.type())) {
                if (instruction.dst.type()==TypedType::F32) {
                    physical_index=instruction.aux/2u;
                    physical_component=static_cast<uint8_t>(instruction.aux&1u);
                } else {
                    if (instruction.aux & 1u) { error="float-vector uniform word offset is not register aligned"; return false; }
                    if (instruction.dst.type()==TypedType::F32x4 && (instruction.aux & 3u)) {
                        error="float4 uniform word offset is not vec4 aligned";
                        return false;
                    }
                    physical_index=instruction.aux/2u;
                }
            }
            if (physical_index >= 128) { error = "resource physical index exceeds current register subset"; return false; }
            values[instruction.dst.id()] = machine.physical(bank, static_cast<uint8_t>(physical_index), type,
                                                            physical_component);
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
        case TypedOpcode::StateInit: {
            if (instruction.dst.type()==TypedType::F32x4 && instruction.src0.type()==TypedType::F32x4) {
                const auto src=lower_value(typed,instruction.src0,values,literals,machine);
                const auto dst=machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
                if (src.kind()==MachineOperandKind::None || dst.kind()==MachineOperandKind::None ||
                    !machine.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                        machine_move_config(0xF),dst,src)) {
                    error="failed to initialize mutable float4 state";
                    return false;
                }
                values[instruction.dst.id()]=dst;
            } else if (instruction.dst.type()==TypedType::S32 && instruction.src0.type()==TypedType::S32 &&
                       instruction.src0.kind()==TypedValueKind::Literal &&
                       instruction.src0.id()<typed.literals().size() && typed.literals()[instruction.src0.id()]==0) {
                const auto dst=machine.make_value<MachineType::S32>();
                if (dst.kind()==MachineOperandKind::None || !machine.emit<MachineOpcode::LoopCounterInit>(0,dst)) {
                    error="failed to initialize oracle loop counter state";
                    return false;
                }
                values[instruction.dst.id()]=dst;
            } else {
                error="typed state init supports only float4 copy or S32 literal zero";
                return false;
            }
            value_types[instruction.dst.id()]=instruction.dst.type();
            value_defined[instruction.dst.id()]=true;
            break;
        }
        case TypedOpcode::StateUpdate: {
            if (instruction.dst.type()!=TypedType::F32x4 || instruction.src0.type()!=TypedType::F32x4) {
                error="typed state update currently supports only float4 loop state";
                return false;
            }
            const auto state=values[instruction.dst.id()];
            const auto src=lower_value(typed,instruction.src0,values,literals,machine);
            if (state.kind()!=MachineOperandKind::VirtualValue || src.kind()==MachineOperandKind::None ||
                !machine.emit_config<MachineOpcode::MoveUpdate>(static_cast<uint8_t>(usse::DataType::F32),
                    machine_move_config(0xF),state,src)) {
                error="failed to update mutable float4 loop state";
                return false;
            }
            break;
        }
        case TypedOpcode::IntIncrement: {
            const uint8_t step=instruction.subop();
            if (instruction.dst.type()!=TypedType::S32 || step<1 || step>3 ||
                instruction.dst.id()>=values.size() || values[instruction.dst.id()].kind()!=MachineOperandKind::VirtualValue ||
                !machine.emit<MachineOpcode::LoopIncrement>(step,values[instruction.dst.id()])) {
                error="typed integer increment is outside oracle loop step 1..3 subset";
                return false;
            }
            break;
        }
        case TypedOpcode::StoreOutput: {
            if (ignore_output_stores) break;
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
            const uint8_t components=typed_component_count(instruction.dst.type());
            const bool scalar=instruction.dst.type()==TypedType::F32 &&
                instruction.src0.type()==TypedType::F32 && instruction.src1.type()==TypedType::F32;
            const bool vector = typed_is_float(instruction.dst.type()) && components>=2 && components<=4 &&
                instruction.src0.type()==instruction.dst.type() && instruction.src1.type()==instruction.dst.type() &&
                (instruction.dst.type()==TypedType::F32x2 || instruction.dst.type()==TypedType::F32x3 ||
                 instruction.dst.type()==TypedType::F32x4);
            const bool dot = float_op == TypedFloatOp::Dot && instruction.dst.type() == TypedType::F32 &&
                instruction.src0.type() == TypedType::F32x4 && instruction.src1.type() == TypedType::F32x4;
            if ((!scalar && !vector && !dot) || float_op > TypedFloatOp::Div ||
                (float_op==TypedFloatOp::Div && !scalar)) {
                error = "typed float binary currently supports scalar/F32-vector arithmetic and float4 dot";
                return false;
            }

            auto src0 = lower_value(typed, instruction.src0, values, literals, machine);
            auto src1 = lower_value(typed, instruction.src1, values, literals, machine);
            const uint8_t width=dot ? 2 : static_cast<uint8_t>(components>2 ? 2 : 1);
            const auto dst = machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, width);
            if (dst.kind() == MachineOperandKind::None || src0.kind() == MachineOperandKind::None ||
                src1.kind() == MachineOperandKind::None) {
                error = "failed to create machine operands for typed float operation";
                return false;
            }

            if (float_op==TypedFloatOp::Div) {
                const auto reciprocal=machine.make_value<MachineType::F32>();
                if (reciprocal.kind()==MachineOperandKind::None ||
                    !machine.emit<MachineOpcode::ComplexF32>(static_cast<uint8_t>(usse::ComplexOp::Reciprocal),
                        reciprocal,src1) ||
                    !machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
                        machine_vector_config(1),dst,src0,reciprocal)) {
                    error="failed to lower scalar F32 division through reciprocal VCOMP";
                    return false;
                }
                values[instruction.dst.id()]=dst;
                value_types[instruction.dst.id()]=instruction.dst.type();
                value_defined[instruction.dst.id()]=true;
                break;
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
            case TypedFloatOp::Div: return false;
            }
            if (!machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(machine_op),
                    machine_vector_config(dot ? 0x1 : static_cast<uint8_t>((1u<<components)-1u),
                        MachineVectorSwizzle::Identity, src0_negative),
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
            const uint8_t components=typed_component_count(instruction.dst.type());
            const bool supported_type=(instruction.dst.type()==TypedType::F32 ||
                                       instruction.dst.type()==TypedType::F32x2 ||
                                       instruction.dst.type()==TypedType::F32x3 ||
                                       instruction.dst.type()==TypedType::F32x4) &&
                                      instruction.src0.type()==instruction.dst.type();
            if (!supported_type || unary_op > TypedFloatUnaryOp::Log2 ||
                (unary_op==TypedFloatUnaryOp::Log2 && instruction.dst.type()!=TypedType::F32)) {
                error = "typed float unary currently supports scalar/F32-vector negate/absolute/saturate plus scalar Log2";
                return false;
            }
            const auto src = lower_value(typed, instruction.src0, values, literals, machine);
            const auto zero = machine.physical(machine_immediate(0), MachineType::F32);
            const uint8_t width=static_cast<uint8_t>(components>2 ? 2 : 1);
            const uint8_t mask=static_cast<uint8_t>((1u<<components)-1u);
            MachineOperand dst{};
            if (unary_op==TypedFloatUnaryOp::Log2) {
                dst=machine.make_value<MachineType::F32>();
                if (src.kind()==MachineOperandKind::None || dst.kind()==MachineOperandKind::None ||
                    !machine.emit<MachineOpcode::ComplexF32>(static_cast<uint8_t>(usse::ComplexOp::Log2),dst,src)) {
                    error="failed to lower scalar Log2 to validated VCOMP";
                    return false;
                }
            } else if (unary_op==TypedFloatUnaryOp::Saturate) {
                const auto clamped_low=machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,width);
                dst=machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,width);
                const auto one=machine.physical(machine_special(1),MachineType::F32);
                if (src.kind()==MachineOperandKind::None || clamped_low.kind()==MachineOperandKind::None ||
                    dst.kind()==MachineOperandKind::None ||
                    !machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Max),
                        machine_vector_config(mask),clamped_low,src,zero) ||
                    !machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Min),
                        machine_vector_config(mask,MachineVectorSwizzle::Source2YYYY),dst,clamped_low,one)) {
                    error="failed to lower typed saturate to validated max/min Machine IR";
                    return false;
                }
            } else {
                dst = machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, width);
                if (dst.kind() == MachineOperandKind::None || src.kind() == MachineOperandKind::None ||
                    !machine.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Add),
                        machine_vector_config(mask, MachineVectorSwizzle::Identity,
                            unary_op == TypedFloatUnaryOp::Neg, unary_op == TypedFloatUnaryOp::Abs),
                        dst, src, zero)) {
                    error = "failed to lower typed float unary operation to Machine IR";
                    return false;
                }
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::FloatSwizzle:
        case TypedOpcode::FloatConstant:
            error="high-level float swizzle/constant requires compile_typed_shader";
            return false;
        case TypedOpcode::FloatCompose: {
            if (instruction.dst.type()!=TypedType::F32x4 || instruction.aux>=typed.float4_composites().size()) {
                error="typed float4 compose side-table entry is invalid";
                return false;
            }
            const auto &components=typed.float4_composites()[instruction.aux];
            for (const auto component:components) {
                if (!valid_value_use(typed,component,value_types,value_defined)) {
                    error="typed float4 compose has an unresolved scalar component";
                    return false;
                }
            }
            bool materialize=false;
            for (uint32_t use_index=instruction_index+1;use_index<typed.instructions().size();++use_index) {
                const auto &use=typed.instructions()[use_index];
                if ((use.src0.bits==instruction.dst.bits || use.src1.bits==instruction.dst.bits) &&
                    use.opcode()!=TypedOpcode::StoreOutput) {
                    materialize=true;
                    break;
                }
            }
            if (materialize) {
                const auto dst=machine.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
                if (dst.kind()==MachineOperandKind::None) {
                    error="failed to allocate materialized float4 compose";
                    return false;
                }
                for (uint8_t lane=0;lane<4;++lane) {
                    const auto src=lower_value(typed,components[lane],values,literals,machine);
                    if (src.kind()==MachineOperandKind::None || src.type()!=MachineType::F32) {
                        error="failed to lower float4 compose scalar component";
                        return false;
                    }
                    uint8_t swizzle=0;
                    if (src.kind()==MachineOperandKind::PhysicalValue && src.physical_component()!=0xff)
                        swizzle=src.physical_component();
                    const uint16_t config=machine_move_config(static_cast<uint8_t>(1u<<lane),swizzle);
                    const bool emitted=lane==0 ?
                        machine.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),config,dst,src) :
                        machine.emit_config<MachineOpcode::MoveUpdate>(static_cast<uint8_t>(usse::DataType::F32),config,dst,src);
                    if (!emitted) {
                        error="failed to materialize float4 compose component";
                        return false;
                    }
                }
                values[instruction.dst.id()]=dst;
            }
            value_types[instruction.dst.id()]=TypedType::F32x4;
            value_defined[instruction.dst.id()]=true;
            break;
        }
        case TypedOpcode::FloatExtract: {
            const auto components=typed_component_count(instruction.src0.type());
            if (instruction.dst.type()!=TypedType::F32 || !typed_is_float(instruction.src0.type()) ||
                components<2 || components>4 || instruction.subop()>=components) {
                error = "typed float extract requires a float vector source, valid component and F32 destination";
                return false;
            }
            const auto src=lower_value(typed,instruction.src0,values,literals,machine);
            if (src.kind()==MachineOperandKind::None) {
                error = "failed to lower float extract source";
                return false;
            }
            if (src.kind()==MachineOperandKind::PhysicalValue) {
                const auto reg=src.physical_register();
                values[instruction.dst.id()]=machine.physical(reg,MachineType::F32,instruction.subop());
            } else if (instruction.subop()==0) {
                // X aliases the base F32 register directly for allocated vector values.
                values[instruction.dst.id()]=src;
            } else {
                error="non-X extraction from a virtual float vector is not yet validated";
                return false;
            }
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
        case TypedOpcode::FloatToS32:
            error="typed F32->S32 conversion requires compile_typed_shader oracle profile";
            return false;
        case TypedOpcode::S32ToFloat:
            error="typed S32->F32 conversion requires compile_typed_shader oracle profile";
            return false;
        case TypedOpcode::Bitwise: {
            const bool integer=(instruction.dst.type()==TypedType::U32 || instruction.dst.type()==TypedType::S32) &&
                instruction.src0.type()==instruction.dst.type() && instruction.src1.type()==instruction.dst.type();
            if (!integer ||
                instruction.subop() > static_cast<uint8_t>(usse::BitwiseOp::ArithmeticShiftRight)) {
                error = "typed bitwise currently requires matching scalar U32/S32 operands"; return false;
            }
            const MachineType machine_integer=instruction.dst.type()==TypedType::S32 ? MachineType::S32 : MachineType::U32;
            const auto dst = machine.make_value(machine_integer);
            const auto src0 = lower_value(typed, instruction.src0, values, literals, machine);
            const auto src1 = lower_value(typed, instruction.src1, values, literals, machine);
            if (dst.kind() == MachineOperandKind::None || src0.kind() == MachineOperandKind::None || src1.kind() == MachineOperandKind::None ||
                !machine.emit<MachineOpcode::Bitwise>(instruction.subop(), dst, src0, src1)) {
                error = "failed to lower typed bitwise operation"; return false;
            }
            values[instruction.dst.id()] = dst;
            value_types[instruction.dst.id()] = instruction.dst.type();
            value_defined[instruction.dst.id()] = true;
            break;
        }
        case TypedOpcode::Compare: {
            const bool u32 = instruction.src0.type() == TypedType::U32 && instruction.src1.type() == TypedType::U32;
            const bool f32 = instruction.src0.type() == TypedType::F32 && instruction.src1.type() == TypedType::F32;
            const bool s32 = instruction.src0.type() == TypedType::S32 && instruction.src1.type() == TypedType::S32;
            if ((!u32 && !f32 && !s32) ||
                instruction.subop() > static_cast<uint8_t>(usse::CompareOp::GreaterEqual)) {
                error = "typed compare currently requires matching U32/F32/S32 scalar operands"; return false;
            }
            if (s32 && instruction.subop()!=static_cast<uint8_t>(usse::CompareOp::Less)) {
                error="typed S32 compare currently supports only oracle loop less-than";
                return false;
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
    if (lowered_values) *lowered_values=values;
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
        const auto *desc=descriptor(instruction.opcode());
        if (desc && desc->roles[0]==TypedRole::ValueDef &&
            instruction.dst.kind() == TypedValueKind::Value && instruction.dst.id() < defs.size()) {
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
        std::vector<const TypedResource *> vertex_uniforms;
        for (const auto &resource : resources) {
            if (resource.kind == TypedResourceKind::Input) inputs.push_back(&resource);
            else if (resource.kind == TypedResourceKind::Matrix4) matrices.push_back(&resource);
            else if (resource.kind == TypedResourceKind::Uniform) vertex_uniforms.push_back(&resource);
        }
        std::sort(inputs.begin(), inputs.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
        std::sort(matrices.begin(), matrices.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
        std::sort(vertex_uniforms.begin(), vertex_uniforms.end(), [](const auto *a, const auto *b) { return a->index < b->index; });
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
            uint8_t gxp_semantic=14;
            uint8_t gxp_semantic_index=resource->semantic_index;
            if (resource->semantic==TypedSemantic::Color) gxp_semantic=6;
            else if (resource->semantic==TypedSemantic::Position) gxp_semantic=11;
            else if (resource->semantic==TypedSemantic::None)
                gxp_semantic_index=static_cast<uint8_t>(resource->index);
            vertex_attributes.push_back({shader.resource_name(*resource), components,
                                         static_cast<uint32_t>(resource->index) * 4u,
                                         gxp_semantic,gxp_semantic_index});
        }

        std::unordered_map<uint16_t, uint32_t> matrix_for_resource;
        for (const auto *resource : matrices) {
            const uint16_t resource_id = static_cast<uint16_t>(resource - resources.data());
            matrix_for_resource[resource_id] = static_cast<uint32_t>(vertex_matrices.size());
            vertex_matrices.push_back({shader.resource_name(*resource), resource->index});
        }

        bool position_written = false;
        bool passthrough_position = false;
        bool constructed_position = false;
        bool transformed_position = false;
        bool generic_position = false;
        uint16_t position_compose = std::numeric_limits<uint16_t>::max();
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
                if (def->opcode()==TypedOpcode::Input && instruction.src0.type()==TypedType::F32x4) {
                    const auto it=attribute_for_value.find(instruction.src0.id());
                    if (it==attribute_for_value.end()) {
                        out.error="typed passthrough position is not a vertex input";
                        return false;
                    }
                    passthrough_position=true;
                    position_attribute=it->second;
                } else if (def->opcode() == TypedOpcode::ConstructPosition) {
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
                } else if (def->opcode()==TypedOpcode::FloatCompose &&
                           instruction.src0.type()==TypedType::F32x4 &&
                           def->aux<program.float4_composites().size()) {
                    generic_position=true;
                    position_compose=def->aux;
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
        if (generic_position) {
            if (passthrough_position || constructed_position || transformed_position || !vertex_matrices.empty() ||
                !varying_written || varying_attribute>=vertex_attributes.size() ||
                selected_varying_semantic!=IrVaryingSemantic::Color || vertex_attributes.size()<2 ||
                position_compose>=program.float4_composites().size()) {
                out.error="generic vertex POSITION+COLOR shape is outside the validated Geometrizer subset";
                return false;
            }
            std::vector<IrUniformFloat> uniform_meta;
            uint32_t uniform_words=0;
            for (const auto *uniform:vertex_uniforms) {
                const uint8_t components=typed_component_count(uniform->type);
                if (!typed_is_float(uniform->type) || components<1 || components>4) {
                    out.error="generic vertex profile only accepts F32 scalar/vector uniforms";
                    return false;
                }
                uniform_meta.push_back({shader.resource_name(*uniform),components,uniform->index});
                uniform_words=std::max<uint32_t>(uniform_words,static_cast<uint32_t>(uniform->index)+components);
            }
            // SA registers contain two F32 words. Sony pads the default uniform
            // footprint to that register boundary (3 -> 4, 9 -> 10), not vec4.
            uniform_words=(uniform_words+1u)&~1u;
            if (uniform_words>=254) { out.error="generic vertex uniform footprint exceeds compact SA subset"; return false; }

            MachineProgram primary;
            if (!primary.emit<MachineOpcode::Phase>()) {
                out.error="failed to start generic vertex Machine program";
                return false;
            }
            std::vector<MachineOperand> literal_bindings(program.literals().size());
            std::vector<IrLiteralF32> literal_meta;
            std::unordered_map<uint32_t,uint32_t> literal_index_by_bits;
            auto bind_literal=[&](TypedValue value) -> bool {
                if (value.kind()!=TypedValueKind::Literal || value.type()!=TypedType::F32 ||
                    value.id()>=program.literals().size()) return true;
                if (literal_bindings[value.id()].kind()!=MachineOperandKind::None) return true;
                const uint32_t bits=program.literals()[value.id()];
                if (bits==0) {
                    literal_bindings[value.id()]=primary.physical(machine_immediate(0),MachineType::F32);
                    return true;
                }
                auto [it,inserted]=literal_index_by_bits.emplace(bits,static_cast<uint32_t>(literal_meta.size()));
                if (inserted) literal_meta.push_back({it->second,bits});
                const uint32_t word=uniform_words+it->second;
                if (word>=254) return false;
                literal_bindings[value.id()]=primary.physical(machine_secondary(static_cast<uint8_t>(word/2u)),
                    MachineType::F32,static_cast<uint8_t>(word&1u));
                return true;
            };
            for (const auto &instruction:instructions) {
                if (!bind_literal(instruction.dst) || !bind_literal(instruction.src0) || !bind_literal(instruction.src1)) {
                    out.error="generic vertex literal table exceeds compact SA subset";
                    return false;
                }
            }
            for (const auto &composite:program.float4_composites())
                for (const auto component:composite)
                    if (!bind_literal(component)) {
                        out.error="generic vertex composite literal table exceeds compact SA subset";
                        return false;
                    }

            std::vector<MachineOperand> lowered_values;
            std::string machine_error;
            if (!lower_typed_program_impl(program,primary,machine_error,false,nullptr,nullptr,nullptr,
                    false,std::numeric_limits<uint16_t>::max(),&literal_bindings,&lowered_values,true)) {
                out.error="generic vertex Typed->Machine lowering failed: "+machine_error;
                return false;
            }
            auto lowered=[&](TypedValue value) -> MachineOperand {
                if (value.kind()==TypedValueKind::Value)
                    return value.id()<lowered_values.size()?lowered_values[value.id()]:MachineOperand{};
                if (value.kind()==TypedValueKind::Literal)
                    return value.id()<literal_bindings.size()?literal_bindings[value.id()]:MachineOperand{};
                return {};
            };
            const auto &position_components=program.float4_composites()[position_compose];
            for (uint8_t lane=0;lane<4;++lane) {
                const auto src=lowered(position_components[lane]);
                if (src.kind()==MachineOperandKind::None || src.type()!=MachineType::F32) {
                    out.error="generic vertex POSITION component did not lower to F32";
                    return false;
                }
                uint8_t swizzle=0;
                if (src.kind()==MachineOperandKind::PhysicalValue && src.physical_component()!=0xff)
                    swizzle=src.physical_component();
                if (!primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                        machine_move_config(static_cast<uint8_t>(1u<<(lane&1u)),swizzle),
                        primary.physical(machine_vertex_output(static_cast<uint8_t>(lane/2u)),MachineType::F32),src)) {
                    out.error="failed to append generic vertex POSITION move";
                    return false;
                }
            }
            const auto color_resource=inputs[varying_attribute];
            const auto color=lowered(color_resource->value);
            if (color.kind()==MachineOperandKind::None || color.type()!=MachineType::F32 ||
                !primary.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                    machine_move_config(3,4,1,true,false),
                    primary.physical(machine_vertex_output(2),MachineType::F32),color) ||
                !primary.emit<MachineOpcode::Emit>()) {
                out.error="failed to append generic vertex COLOR/EMIT";
                return false;
            }
            return compile_vertex_generic_machine(primary,vertex_attributes,uniform_meta,literal_meta,
                                                  selected_varying_semantic,0,0,out);
        }
        if (passthrough_position) {
            if (constructed_position || transformed_position || !vertex_matrices.empty() ||
                position_attribute>=vertex_attributes.size()) {
                out.error="typed passthrough-position vertex shape is unsupported";
                return false;
            }
            if (!varying_written) {
                if (vertex_attributes.size()!=1) {
                    out.error="typed passthrough vertex profile requires one attribute";
                    return false;
                }
                return compile_vertex_passthrough(vertex_attributes[position_attribute],0,0,out);
            }
            if (vertex_attributes.size()!=2 || varying_attribute>=vertex_attributes.size() ||
                varying_attribute==position_attribute) {
                out.error="typed passthrough-varying vertex profile requires two distinct attributes";
                return false;
            }
            return compile_vertex_passthrough_varying(vertex_attributes[position_attribute],
                                                      vertex_attributes[varying_attribute],
                                                      selected_varying_semantic,0,0,out);
        }
        if (constructed_position) {
            if (transformed_position || varying_written || vertex_attributes.size()!=1 || !vertex_matrices.empty() ||
                position_attribute>=vertex_attributes.size()) {
                out.error = "typed constructed-position vertex shape is unsupported";
                return false;
            }
            return compile_vertex_construct_position(vertex_attributes[position_attribute],0,0,out);
        }
        if (transformed_position && !varying_written && vertex_attributes.size()==1 && vertex_matrices.size()==1 &&
            position_attribute<vertex_attributes.size() && position_matrix==0 &&
            vertex_attributes[position_attribute].components==4) {
            return compile_vertex_uniform_matrix(vertex_attributes[position_attribute],vertex_matrices[0],0,0,out);
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
    std::vector<IrUniformS32> fragment_s32_uniforms;
    std::vector<IrUniformS32> fragment_i32x2_uniforms;
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
        if (resource->value.kind() != TypedValueKind::Value) {
            out.error="typed fragment uniform has no value handle";
            return false;
        }
        if (resource->type==TypedType::F32x4)
            fragment_uniforms.push_back({shader.resource_name(*resource),resource->index});
        else if (resource->type==TypedType::S32)
            fragment_s32_uniforms.push_back({shader.resource_name(*resource),resource->index});
        else if (resource->type==TypedType::U32x2)
            fragment_i32x2_uniforms.push_back({shader.resource_name(*resource),resource->index});
        else {
            out.error="typed fragment uniform type is outside validated float4/S32/int2 profiles";
            return false;
        }
    }
    for (const auto *resource : samplers)
        fragment_samplers.push_back({shader.resource_name(*resource), resource->index});

    std::vector<const TypedInstruction *> output_stores;
    for (const auto &instruction : instructions)
        if (instruction.opcode() == TypedOpcode::StoreOutput) output_stores.push_back(&instruction);

    if (!program.labels().empty()) {
        const bool loop_control=std::any_of(instructions.begin(),instructions.end(),[](const TypedInstruction &instruction) {
            return instruction.opcode()==TypedOpcode::IntIncrement;
        });
        if (output_stores.empty() || !samplers.empty() || inputs.size()<2 || inputs.size()>3) {
            out.error = "typed fragment control path requires 2-3 float4 inputs and output stores";
            return false;
        }
        if ((!loop_control && !uniforms.empty()) ||
            (loop_control && (inputs.size()!=3 || !fragment_uniforms.empty() || fragment_s32_uniforms.size()!=1 || uniforms.size()!=1))) {
            out.error=loop_control ?
                "typed loop path requires three float4 inputs and one S32 uniform" :
                "typed non-loop control path does not accept uniforms";
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
        if (loop_control)
            return compile_fragment_loop_machine(primary,fragment_s32_uniforms[0],0,0,out);
        return compile_fragment_control_machine(primary,static_cast<uint8_t>(inputs.size()),0,0,out);
    }

    const TypedInstruction *store = nullptr;
    for (const auto *candidate : output_stores) {
        if (store) { out.error = "typed fragment shader has multiple output stores"; return false; }
        store = candidate;
    }
    if (!store) { out.error = "typed fragment shader has no output store"; return false; }
    const TypedValue root = store->src0;

    if (root.type()==TypedType::U32x2) {
        if (!inputs.empty() || !fragment_uniforms.empty() || !fragment_s32_uniforms.empty() ||
            !fragment_samplers.empty() || fragment_i32x2_uniforms.empty() || fragment_i32x2_uniforms.size()>2 ||
            store->aux>=resources.size() || resources[store->aux].kind!=TypedResourceKind::Output ||
            resources[store->aux].index!=0 || resources[store->aux].type!=TypedType::U32x2) {
            out.error="typed int2 output requires Location 0 and one/two packed int2 uniforms";
            return false;
        }
        MachineProgram primary,secondary;
        const auto zero=primary.literal_s32(0);
        if (zero.kind()==MachineOperandKind::None || !primary.emit<MachineOpcode::Phase>() ||
            !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
                primary.physical(machine_fragment_output(0),MachineType::S32),
                primary.physical(machine_secondary(0),MachineType::S32),zero)) {
            out.error="failed to build int2 primary uniform copy";
            return false;
        }
        if (const auto *resource=resource_for_value(root)) {
            if (fragment_i32x2_uniforms.size()!=1 || resource->kind!=TypedResourceKind::Uniform ||
                resource->type!=TypedType::U32x2 || resource->index!=0) {
                out.error="direct int2 output currently requires uniform resource 0";
                return false;
            }
        } else {
            const auto *def=definition(root);
            auto uniform_index=[&](TypedValue value) -> int {
                const auto *resource=resource_for_value(value);
                return resource && resource->kind==TypedResourceKind::Uniform && resource->type==TypedType::U32x2 ?
                    static_cast<int>(resource->index) : -1;
            };
            if (!def || def->opcode()!=TypedOpcode::Bitwise ||
                def->subop()!=static_cast<uint8_t>(usse::BitwiseOp::Or) ||
                fragment_i32x2_uniforms.size()!=2 ||
                !((uniform_index(def->src0)==0 && uniform_index(def->src1)==2) ||
                  (uniform_index(def->src0)==2 && uniform_index(def->src1)==0))) {
                out.error="int2 output currently covers only OR of packed uniform resources 0 and 2";
                return false;
            }
            if (!secondary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
                    secondary.physical(machine_primary(1),MachineType::S32),
                    secondary.physical(machine_primary(3),MachineType::S32),
                    secondary.physical(machine_primary(1),MachineType::S32)) ||
                !secondary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
                    secondary.physical(machine_primary(0),MachineType::S32),
                    secondary.physical(machine_primary(2),MachineType::S32),
                    secondary.physical(machine_primary(0),MachineType::S32))) {
                out.error="failed to build oracle int2 OR secondary operations";
                return false;
            }
        }
        if (!secondary.emit<MachineOpcode::S32x2ColorPack>()) {
            out.error="failed to append oracle int2 COLOR pack";
            return false;
        }
        return compile_fragment_s32x2_machine(primary,secondary,fragment_i32x2_uniforms,0,0,out);
    }

    if (root.type()==TypedType::S32) {
        const bool s32_output=store->aux<resources.size() &&
            resources[store->aux].kind==TypedResourceKind::Output &&
            resources[store->aux].index==0 && resources[store->aux].type==TypedType::S32;
        if (s32_output && uniforms.empty() && samplers.empty() && inputs.size()==1 && inputs[0]->index==0) {
            bool conversion_profile=false;
            if (const auto *resource=resource_for_value(root)) {
                conversion_profile=resource->kind==TypedResourceKind::Input && resource->type==TypedType::S32;
            } else if (const auto *def=definition(root); def && def->opcode()==TypedOpcode::FloatToS32) {
                const auto *source=resource_for_value(def->src0);
                conversion_profile=source && source->kind==TypedResourceKind::Input &&
                    source->type==TypedType::F32 && source->index==0;
            }
            if (conversion_profile) {
                MachineProgram primary;
                if (!primary.emit<MachineOpcode::Phase>() ||
                    !primary.emit<MachineOpcode::F32ToS32Color>(0,
                        primary.physical(machine_fragment_output(0),MachineType::S32),
                        primary.physical(machine_primary(0),MachineType::F32,0))) {
                    out.error="failed to build oracle F32->S32 fragment Machine profile";
                    return false;
                }
                return compile_fragment_f32_to_s32_machine(primary,0,0,out);
            }
        }
        if (!inputs.empty() || !fragment_uniforms.empty() || !fragment_samplers.empty() ||
            fragment_s32_uniforms.empty() || fragment_s32_uniforms.size()>2 ||
            !s32_output) {
            out.error="typed scalar S32 output requires Location 0, one/two S32 uniforms and no other resources";
            return false;
        }
        for (size_t i=0;i<fragment_s32_uniforms.size();++i) {
            if (fragment_s32_uniforms[i].resource_index!=i) {
                out.error="typed scalar S32 uniforms must be contiguous from resource 0";
                return false;
            }
        }

        MachineProgram primary,secondary;
        const auto zero=primary.literal_s32(0);
        if (zero.kind()==MachineOperandKind::None || !primary.emit<MachineOpcode::Phase>() ||
            !primary.emit<MachineOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),
                primary.physical(machine_fragment_output(0),MachineType::S32),
                primary.physical(machine_secondary(0),MachineType::S32),zero)) {
            out.error="failed to build scalar S32 primary uniform copy";
            return false;
        }

        if (const auto *resource=resource_for_value(root)) {
            if (resource->kind!=TypedResourceKind::Uniform || resource->type!=TypedType::S32 || resource->index!=0) {
                out.error="direct scalar S32 output currently requires uniform resource 0";
                return false;
            }
        } else {
            const auto *def=definition(root);
            if (!def || def->opcode()!=TypedOpcode::Bitwise ||
                def->subop()>static_cast<uint8_t>(usse::BitwiseOp::ArithmeticShiftRight)) {
                out.error="scalar S32 output producer is outside the validated bitwise subset";
                return false;
            }
            auto uniform_index=[&](TypedValue value) -> int {
                const auto *resource=resource_for_value(value);
                return resource && resource->kind==TypedResourceKind::Uniform && resource->type==TypedType::S32 ?
                    static_cast<int>(resource->index) : -1;
            };
            const int lhs_uniform=uniform_index(def->src0);
            const int rhs_uniform=uniform_index(def->src1);
            const bool lhs_literal=def->src0.kind()==TypedValueKind::Literal && def->src0.id()<program.literals().size();
            const bool rhs_literal=def->src1.kind()==TypedValueKind::Literal && def->src1.id()<program.literals().size();
            MachineOperand lhs{},rhs{};
            if (lhs_uniform>=0 && rhs_uniform>=0) {
                const auto op=static_cast<usse::BitwiseOp>(def->subop());
                if (op!=usse::BitwiseOp::Or || fragment_s32_uniforms.size()!=2 ||
                    lhs_uniform==rhs_uniform || lhs_uniform>1 || rhs_uniform>1) {
                    out.error="register-register scalar S32 profile currently covers oracle OR of uniforms 0/1";
                    return false;
                }
                // Canonical Sony word is OR PA1, PA0 -> PA0 regardless of source spelling.
                lhs=secondary.physical(machine_primary(1),MachineType::S32);
                rhs=secondary.physical(machine_primary(0),MachineType::S32);
            } else {
                if (fragment_s32_uniforms.size()!=1) {
                    out.error="immediate scalar S32 bitwise profile requires one uniform";
                    return false;
                }
                const auto op=static_cast<usse::BitwiseOp>(def->subop());
                const bool commutative=op==usse::BitwiseOp::And || op==usse::BitwiseOp::Or || op==usse::BitwiseOp::Xor;
                uint32_t literal=0;
                if (lhs_uniform==0 && rhs_literal) {
                    lhs=secondary.physical(machine_primary(0),MachineType::S32);
                    literal=program.literals()[def->src1.id()];
                } else if (commutative && rhs_uniform==0 && lhs_literal) {
                    lhs=secondary.physical(machine_primary(0),MachineType::S32);
                    literal=program.literals()[def->src0.id()];
                } else {
                    out.error="scalar S32 immediate bitwise operands are outside the validated uniform/literal shape";
                    return false;
                }
                rhs=secondary.literal_s32(static_cast<int32_t>(literal));
                if (rhs.kind()==MachineOperandKind::None) {
                    out.error="failed to materialize scalar S32 bitwise immediate";
                    return false;
                }
            }
            if (!secondary.emit<MachineOpcode::Bitwise>(def->subop(),
                    secondary.physical(machine_fragment_output(0),MachineType::S32),lhs,rhs)) {
                out.error="failed to build scalar S32 secondary bitwise operation";
                return false;
            }
        }

        if (!secondary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::S16,usse::PackFormat::F16),
                machine_pack_config(1,true,false,true),
                secondary.physical(machine_fragment_output(0),MachineType::F16),
                secondary.physical(machine_primary(0),MachineType::S32),
                secondary.physical(machine_immediate(0),MachineType::S32))) {
            out.error="failed to append oracle scalar S32 output pack";
            return false;
        }
        return compile_fragment_s32_machine(primary,secondary,fragment_s32_uniforms,0,0,out);
    }

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
    if (root_def->opcode()==TypedOpcode::S32ToFloat) {
        const auto *source=resource_for_value(root_def->src0);
        if (root.type()!=TypedType::F32 || !source || source->kind!=TypedResourceKind::Uniform ||
            source->type!=TypedType::S32 || source->index!=0 || inputs.size()!=0 ||
            uniforms.size()!=1 || fragment_s32_uniforms.size()!=1 || !fragment_uniforms.empty() ||
            !samplers.empty() || store->aux>=resources.size() ||
            resources[store->aux].kind!=TypedResourceKind::Output || resources[store->aux].index!=0 ||
            resources[store->aux].type!=TypedType::F32) {
            out.error="typed S32->F32 profile requires one S32 uniform and scalar F32 COLOR0 output";
            return false;
        }

        MachineProgram primary,secondary;
        if (!secondary.emit<MachineOpcode::S32ToF32Scalar>(0,
                secondary.physical(machine_primary(0),MachineType::F32),
                secondary.physical(machine_primary(0),MachineType::S32))) {
            out.error="failed to build oracle S32->F32 secondary conversion";
            return false;
        }
        if (!primary.emit<MachineOpcode::Phase>() ||
            !primary.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                machine_pack_config(1,true,false,false),
                primary.physical(machine_fragment_output(0),MachineType::F16),
                primary.physical(machine_secondary(0),MachineType::F32),
                primary.physical(machine_immediate(0),MachineType::F32))) {
            out.error="failed to build oracle S32->F32 primary output pack";
            return false;
        }
        return compile_fragment_s32_to_f32_machine(primary,secondary,fragment_s32_uniforms[0],0,0,out);
    }
    if (root_def->opcode() == TypedOpcode::Sample2D) {
        return compile_fragment_machine_profile(FragmentMachineProfile::Texture2D,
                                                fragment_uniforms,fragment_samplers,0,0,out);
    }
    if (root_def->opcode()==TypedOpcode::FloatBinary &&
        root_def->subop()==static_cast<uint8_t>(TypedFloatOp::Div)) {
        if (!uniforms.empty() || !samplers.empty()) {
            out.error="typed direct F32 division profile does not accept uniforms/samplers";
            return false;
        }
        const TypedType div_type=root.type();
        const uint8_t components=typed_component_count(div_type);
        if ((div_type!=TypedType::F32 && div_type!=TypedType::F32x2 &&
             div_type!=TypedType::F32x3 && div_type!=TypedType::F32x4) ||
            components<1 || components>4) {
            out.error="typed direct F32 division requires width 1..4";
            return false;
        }
        const auto *numerator=resource_for_value(root_def->src0);
        const auto *denominator=resource_for_value(root_def->src1);
        if (!numerator || !denominator || numerator->kind!=TypedResourceKind::Input ||
            denominator->kind!=TypedResourceKind::Input || numerator->type!=div_type ||
            denominator->type!=div_type || numerator->index>2 || denominator->index>2 ||
            numerator->index==denominator->index) {
            out.error="typed F32 division currently requires two distinct direct matching inputs";
            return false;
        }
        if (store->aux>=resources.size() || resources[store->aux].kind!=TypedResourceKind::Output ||
            resources[store->aux].index!=0 || resources[store->aux].type!=div_type) {
            out.error="typed F32 division output must match the vector result at Location 0";
            return false;
        }
        const uint8_t input_count=static_cast<uint8_t>(std::max(numerator->index,denominator->index)+1);
        auto pa_base=[&](uint16_t location) {
            if (components==1) return static_cast<uint8_t>(location/2u);
            return static_cast<uint8_t>(components==2 ? location : location*2u);
        };
        auto pa_component=[&](uint16_t location) {
            return components==1 ? static_cast<uint8_t>(location&1u) : uint8_t{0xff};
        };
        MachineProgram primary;
        if (!primary.emit<MachineOpcode::Phase>() || !primary.emit<MachineOpcode::Nop>() ||
            !primary.emit<MachineOpcode::DivF32>(components,
                primary.physical(machine_fragment_output(0),MachineType::F16),
                primary.physical(machine_primary(pa_base(numerator->index)),MachineType::F32,
                                 pa_component(numerator->index)),
                primary.physical(machine_primary(pa_base(denominator->index)),MachineType::F32,
                                 pa_component(denominator->index)))) {
            out.error="failed to build oracle F32 division Machine profile";
            return false;
        }
        return compile_fragment_arithmetic_machine(primary,{},input_count,components,0,0,out);
    }
    if (root_def->opcode()==TypedOpcode::FloatSplat &&
        (root.type()==TypedType::F32x2 || root.type()==TypedType::F32x3)) {
        const auto *dot=definition(root_def->src0);
        if (dot && dot->opcode()==TypedOpcode::FloatBinary &&
            dot->subop()==static_cast<uint8_t>(TypedFloatOp::Dot)) {
            if (!uniforms.empty() || !samplers.empty()) {
                out.error="typed narrow dot-splat profile does not accept uniforms/samplers";
                return false;
            }
            const uint8_t components=typed_component_count(root.type());
            const auto *lhs=resource_for_value(dot->src0);
            const auto *rhs=resource_for_value(dot->src1);
            if (!lhs || !rhs || lhs->kind!=TypedResourceKind::Input || rhs->kind!=TypedResourceKind::Input ||
                lhs->type!=root.type() || rhs->type!=root.type() || lhs->index!=0 || rhs->index!=1 ||
                store->aux>=resources.size() || resources[store->aux].kind!=TypedResourceKind::Output ||
                resources[store->aux].index!=0 || resources[store->aux].type!=root.type()) {
                out.error="typed narrow dot-splat requires direct Location0/1 inputs and matching output";
                return false;
            }
            const uint8_t rhs_pa=components==2 ? 1 : 2;
            MachineProgram primary;
            if (!primary.emit<MachineOpcode::Phase>() || !primary.emit<MachineOpcode::Nop>() ||
                !primary.emit<MachineOpcode::DotSplatF32>(components,
                    primary.physical(machine_fragment_output(0),MachineType::F16),
                    primary.physical(machine_primary(0),MachineType::F32),
                    primary.physical(machine_primary(rhs_pa),MachineType::F32))) {
                out.error="failed to build oracle narrow dot-splat Machine profile";
                return false;
            }
            return compile_fragment_arithmetic_machine(primary,{},2,components,0,0,out);
        }
    }
    if (root_def->opcode()==TypedOpcode::FloatSwizzle &&
        root_def->subop()==static_cast<uint8_t>(TypedFloatSwizzleOp::Wzyx) &&
        inputs.size()==1 && inputs[0]->index==0 && inputs[0]->type==TypedType::F32x4 &&
        uniforms.empty() && samplers.empty()) {
        return compile_fragment_machine_profile(FragmentMachineProfile::SwizzleWzyx,{}, {},0,0,out);
    }
    if (root_def->opcode()==TypedOpcode::FloatConstant && uniforms.empty() && samplers.empty() && inputs.empty()) {
        if (root_def->aux>=program.float4_literals().size()) {
            out.error="typed float constant index is out of range";
            return false;
        }
        const std::array<uint32_t,4> red={{0x3f800000u,0u,0u,0x3f800000u}};
        if (program.float4_literals()[root_def->aux]!=red) {
            out.error="float4 constant is outside the oracle-validated red profile";
            return false;
        }
        return compile_fragment_machine_profile(FragmentMachineProfile::ConstantRed,{}, {},0,0,out);
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
    std::vector<uint8_t> reachable(program.value_count(),0);
    std::vector<uint16_t> used_input_locations;
    std::function<void(TypedValue)> collect_inputs=[&](TypedValue value) {
        if (value.kind()!=TypedValueKind::Value || value.id()>=reachable.size() || reachable[value.id()]) return;
        reachable[value.id()]=1;
        if (const auto *resource=resource_for_value(value); resource && resource->kind==TypedResourceKind::Input) {
            used_input_locations.push_back(resource->index);
            return;
        }
        const auto *def=definition(value);
        if (!def) return;
        collect_inputs(def->src0);
        collect_inputs(def->src1);
    };
    collect_inputs(root);
    std::sort(used_input_locations.begin(),used_input_locations.end());
    used_input_locations.erase(std::unique(used_input_locations.begin(),used_input_locations.end()),used_input_locations.end());
    const TypedType arithmetic_type=root.type();
    const uint8_t arithmetic_components=typed_component_count(arithmetic_type);
    if ((arithmetic_type!=TypedType::F32 && arithmetic_type!=TypedType::F32x2 &&
         arithmetic_type!=TypedType::F32x3 && arithmetic_type!=TypedType::F32x4) ||
        used_input_locations.empty() || used_input_locations.size()>3) {
        out.error="typed generic arithmetic path requires one to three reachable F32 inputs (width 1..4)";
        return false;
    }
    for (size_t i=0;i<used_input_locations.size();++i) {
        if (used_input_locations[i]!=i) {
            out.error="typed generic arithmetic inputs must occupy contiguous locations from zero";
            return false;
        }
        const auto found=std::find_if(inputs.begin(),inputs.end(),[&](const TypedResource *resource) {
            return resource->index==i;
        });
        if (found==inputs.end() || (*found)->type!=arithmetic_type) {
            out.error="typed generic arithmetic reachable input type does not match output vector width";
            return false;
        }
    }
    for (size_t i=0;i<uniforms.size();++i) {
        if (uniforms[i]->index != i*4u) {
            out.error = "typed arithmetic float4 uniforms must use contiguous word offsets";
            return false;
        }
    }
    if (store->aux >= resources.size() || resources[store->aux].kind != TypedResourceKind::Output ||
        resources[store->aux].index != 0 || resources[store->aux].type != arithmetic_type) {
        out.error = "typed generic arithmetic output must match the F32 vector result at Location 0";
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
    if (stored_resource != store->aux || stored_type != arithmetic_type ||
        stored_value.kind() == MachineOperandKind::None) {
        out.error = "typed arithmetic Machine IR did not produce the expected vector output";
        return false;
    }
    const uint8_t output_mask=static_cast<uint8_t>((1u<<arithmetic_components)-1u);
    bool packed=false;
    if (arithmetic_components<=2) {
        packed=primary.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
            machine_pack_config(output_mask,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16),stored_value,
            primary.physical(machine_immediate(0),MachineType::F32));
    } else {
        packed=primary.emit_config<MachineOpcode::PackValue>(
            machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
            machine_pack_config(output_mask,true,false),
            primary.physical(machine_fragment_output(0),MachineType::F16),stored_value);
    }
    if (!packed) {
        out.error = "failed to append typed arithmetic output pack";
        return false;
    }
    return compile_fragment_arithmetic_machine(primary,fragment_uniforms,
                                               static_cast<uint8_t>(used_input_locations.size()),
                                               arithmetic_components,0,0,out);
}

} // namespace vsc::backend
