#include "spirv/spirv_cross_adapter.hpp"

#include <unordered_map>
#include <unordered_set>

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
#include <spirv_cross/spirv_cross.hpp>
#endif

namespace vsc {

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
namespace {

bool is_scalar_u32(const spirv_cross::Compiler &compiler, uint32_t type_id) {
    const auto &type = compiler.get_type(type_id);
    return type.basetype == spirv_cross::SPIRType::UInt && type.width == 32 &&
        type.vecsize == 1 && type.columns == 1 && type.array.empty();
}

bool lookup(const std::unordered_map<uint32_t, backend::TypedValue> &values,
            uint32_t id, backend::TypedValue &out) {
    const auto it = values.find(id);
    if (it == values.end()) return false;
    out = it->second;
    return true;
}

bool map_bitwise(uint16_t op, usse::BitwiseOp &mapped) {
    switch (static_cast<spv::Op>(op)) {
    case spv::OpBitwiseAnd: mapped = usse::BitwiseOp::And; return true;
    case spv::OpBitwiseOr: mapped = usse::BitwiseOp::Or; return true;
    case spv::OpBitwiseXor: mapped = usse::BitwiseOp::Xor; return true;
    case spv::OpShiftLeftLogical: mapped = usse::BitwiseOp::ShiftLeft; return true;
    case spv::OpShiftRightLogical: mapped = usse::BitwiseOp::ShiftRight; return true;
    case spv::OpShiftRightArithmetic: mapped = usse::BitwiseOp::ArithmeticShiftRight; return true;
    default: return false;
    }
}

bool map_compare(uint16_t op, usse::CompareOp &mapped) {
    switch (static_cast<spv::Op>(op)) {
    case spv::OpIEqual: mapped = usse::CompareOp::Equal; return true;
    case spv::OpINotEqual: mapped = usse::CompareOp::NotEqual; return true;
    case spv::OpULessThan: mapped = usse::CompareOp::Less; return true;
    case spv::OpULessThanEqual: mapped = usse::CompareOp::LessEqual; return true;
    case spv::OpUGreaterThan: mapped = usse::CompareOp::Greater; return true;
    case spv::OpUGreaterThanEqual: mapped = usse::CompareOp::GreaterEqual; return true;
    default: return false;
    }
}

} // namespace
#endif

bool spirv_cross_to_typed_fragment(const std::vector<uint32_t> &words,
                                   const char *entrypoint,
                                   backend::TypedProgram &typed,
                                   std::string &error) {
    typed = {};
    error.clear();
#if !defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    (void)words;
    (void)entrypoint;
    error = "SPIRV-Cross adapter is disabled";
    return false;
#else
    try {
        spirv_cross::Compiler compiler(words);
        const std::string name = (entrypoint && *entrypoint) ? entrypoint : "main";
        compiler.set_entry_point(name, spv::ExecutionModelFragment);
        const uint32_t function_id = compiler.get_entry_point(name, spv::ExecutionModelFragment).self;
        const auto resources = compiler.get_shader_resources();

        std::unordered_map<uint32_t, backend::TypedValue> values;
        for (const auto &resource : resources.stage_inputs) {
            if (!is_scalar_u32(compiler, resource.type_id) ||
                !compiler.has_decoration(resource.id, spv::DecorationLocation))
                continue;
            const uint32_t location = compiler.get_decoration(resource.id, spv::DecorationLocation);
            if (location >= 128) { error = "stage input location exceeds current Typed IR register subset"; return false; }
            const auto value = typed.input<backend::TypedType::U32>(static_cast<uint16_t>(location));
            if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR input"; return false; }
            values[resource.id] = value;
        }

        // Constants are global, so collect the scalar U32 subset before walking the entry function.
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t first = words[offset];
            const uint16_t count = static_cast<uint16_t>(first >> 16);
            const uint16_t op = static_cast<uint16_t>(first);
            if (!count || offset + count > words.size()) { error = "malformed SPIR-V instruction stream"; return false; }
            const uint32_t *args = words.data() + offset + 1;
            if (op == spv::OpConstant && count == 4 && is_scalar_u32(compiler, args[0])) {
                values[args[1]] = typed.literal_u32(args[2]);
            }
            offset += count;
        }

        std::unordered_set<uint32_t> kill_labels;
        bool in_function = false;
        uint32_t current_label = 0;
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t first = words[offset];
            const uint16_t count = static_cast<uint16_t>(first >> 16);
            const uint16_t op = static_cast<uint16_t>(first);
            if (!count || offset + count > words.size()) { error = "malformed SPIR-V instruction stream"; return false; }
            const uint32_t *args = words.data() + offset + 1;
            if (op == spv::OpFunction && count >= 3) in_function = args[1] == function_id;
            else if (in_function && op == spv::OpLabel && count == 2) current_label = args[0];
            else if (in_function && op == spv::OpKill && current_label) kill_labels.insert(current_label);
            else if (in_function && op == spv::OpFunctionEnd) { in_function = false; current_label = 0; }
            offset += count;
        }

        if (kill_labels.empty()) { error = "Typed IR adapter found no fragment discard path"; return false; }

        bool emitted_discard = false;
        in_function = false;
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t first = words[offset];
            const uint16_t count = static_cast<uint16_t>(first >> 16);
            const uint16_t op = static_cast<uint16_t>(first);
            if (!count || offset + count > words.size()) { error = "malformed SPIR-V instruction stream"; return false; }
            const uint32_t *args = words.data() + offset + 1;

            if (op == spv::OpFunction && count >= 3) {
                in_function = args[1] == function_id;
                offset += count;
                continue;
            }
            if (!in_function) { offset += count; continue; }
            if (op == spv::OpFunctionEnd) break;

            if (op == spv::OpLoad && count >= 4) {
                backend::TypedValue source{};
                if (!is_scalar_u32(compiler, args[0]) || !lookup(values, args[2], source)) {
                    error = "Typed IR adapter supports only direct scalar U32 resource loads";
                    return false;
                }
                values[args[1]] = source;
            } else {
                usse::BitwiseOp bitwise{};
                usse::CompareOp compare{};
                if (map_bitwise(op, bitwise)) {
                    if (count != 5 || !is_scalar_u32(compiler, args[0])) { error = "invalid scalar U32 bitwise instruction"; return false; }
                    backend::TypedValue lhs{}, rhs{};
                    if (!lookup(values, args[2], lhs) || !lookup(values, args[3], rhs)) { error = "unresolved bitwise operand"; return false; }
                    const auto dst = typed.make_value<backend::TypedType::U32>();
                    if (!typed.emit<backend::TypedOpcode::Bitwise>(static_cast<uint8_t>(bitwise), dst, lhs, rhs)) {
                        error = "failed to emit Typed IR bitwise instruction"; return false;
                    }
                    values[args[1]] = dst;
                } else if (map_compare(op, compare)) {
                    if (count != 5) { error = "invalid U32 compare instruction"; return false; }
                    backend::TypedValue lhs{}, rhs{};
                    if (!lookup(values, args[2], lhs) || !lookup(values, args[3], rhs) ||
                        lhs.type() != backend::TypedType::U32 || rhs.type() != backend::TypedType::U32) {
                        error = "unresolved U32 compare operand"; return false;
                    }
                    const auto dst = typed.make_predicate();
                    if (!typed.emit<backend::TypedOpcode::Compare>(static_cast<uint8_t>(compare), dst, lhs, rhs)) {
                        error = "failed to emit Typed IR compare"; return false;
                    }
                    values[args[1]] = dst;
                } else if (op == spv::OpBranchConditional) {
                    if (count != 4) { error = "invalid conditional branch"; return false; }
                    const bool true_kills = kill_labels.count(args[1]) != 0;
                    const bool false_kills = kill_labels.count(args[2]) != 0;
                    if (true_kills == false_kills) { error = "conditional branch is not a single discard edge"; return false; }
                    backend::TypedValue predicate{};
                    if (!lookup(values, args[0], predicate) || predicate.kind() != backend::TypedValueKind::Predicate) {
                        error = "discard branch condition is not a Typed IR predicate"; return false;
                    }
                    if (false_kills) predicate = backend::TypedValue::predicate(predicate.id(), !predicate.inverted());
                    if (!typed.emit<backend::TypedOpcode::Discard>(0, {}, predicate)) {
                        error = "failed to emit Typed IR discard"; return false;
                    }
                    emitted_discard = true;
                } else if (op != spv::OpLabel && op != spv::OpSelectionMerge && op != spv::OpBranch &&
                           op != spv::OpKill && op != spv::OpReturn && op != spv::OpNop) {
                    error = "unsupported instruction in SPIRV-Cross Typed IR fragment subset";
                    return false;
                }
            }
            offset += count;
        }

        if (!emitted_discard) { error = "Typed IR adapter did not emit discard"; return false; }
        return true;
    } catch (const std::exception &e) {
        error = std::string("SPIRV-Cross Typed IR adapter: ") + e.what();
        return false;
    }
#endif
}

} // namespace vsc
