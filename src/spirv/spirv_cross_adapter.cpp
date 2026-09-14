#include "spirv/spirv_cross_adapter.hpp"

#include <algorithm>
#include <cctype>
#include <limits>
#include <unordered_map>
#include <unordered_set>

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
#include <spirv/unified1/GLSL.std.450.h>
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

backend::TypedType typed_type(const spirv_cross::SPIRType &type) {
    if (!type.array.empty() || type.columns != 1) return backend::TypedType::Invalid;
    auto vector_type = [&](backend::TypedType scalar, backend::TypedType v2,
                           backend::TypedType v3, backend::TypedType v4) {
        switch (type.vecsize) {
        case 1: return scalar;
        case 2: return v2;
        case 3: return v3;
        case 4: return v4;
        default: return backend::TypedType::Invalid;
        }
    };
    if (type.basetype == spirv_cross::SPIRType::Float && type.width == 32)
        return vector_type(backend::TypedType::F32, backend::TypedType::F32x2,
                           backend::TypedType::F32x3, backend::TypedType::F32x4);
    if ((type.basetype == spirv_cross::SPIRType::Half ||
         (type.basetype == spirv_cross::SPIRType::Float && type.width == 16)))
        return vector_type(backend::TypedType::F16, backend::TypedType::F16x2,
                           backend::TypedType::F16x3, backend::TypedType::F16x4);
    if (type.basetype == spirv_cross::SPIRType::UInt && type.width == 32)
        return vector_type(backend::TypedType::U32, backend::TypedType::U32x2,
                           backend::TypedType::U32x3, backend::TypedType::U32x4);
    if (type.basetype == spirv_cross::SPIRType::UInt && type.width == 16 && type.vecsize == 1)
        return backend::TypedType::U16;
    if (type.basetype == spirv_cross::SPIRType::Int && type.width == 32 && type.vecsize == 1)
        return backend::TypedType::S32;
    return backend::TypedType::Invalid;
}

bool is_f32_mat4(const spirv_cross::SPIRType &type) {
    return type.basetype == spirv_cross::SPIRType::Float && type.width == 32 &&
        type.vecsize == 4 && type.columns == 4 && type.array.empty();
}

backend::TypedSemantic semantic_from_text(const std::string &text, uint8_t &index) {
    std::string upper;
    upper.reserve(text.size());
    for (char c : text) upper.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    size_t split = upper.size();
    while (split && std::isdigit(static_cast<unsigned char>(upper[split - 1]))) --split;
    index = 0;
    if (split < upper.size()) {
        unsigned value = 0;
        for (size_t i = split; i < upper.size(); ++i) value = value * 10u + static_cast<unsigned>(upper[i] - '0');
        index = static_cast<uint8_t>(std::min(value, 255u));
    }
    const std::string base = upper.substr(0, split);
    if (base.find("POSITION") != std::string::npos) return backend::TypedSemantic::Position;
    if (base.find("COLOR") != std::string::npos || base.find("COLOUR") != std::string::npos)
        return backend::TypedSemantic::Color;
    if (base.find("TEXCOORD") != std::string::npos || base == "UV")
        return backend::TypedSemantic::TexCoord;
    return backend::TypedSemantic::None;
}

backend::TypedSemantic reflected_semantic(const spirv_cross::Compiler &compiler,
                                          uint32_t id, const std::string &name,
                                          uint8_t &index) {
    index = 0;
    if (compiler.has_decoration(id, spv::DecorationBuiltIn) &&
        compiler.get_decoration(id, spv::DecorationBuiltIn) == spv::BuiltInPosition)
        return backend::TypedSemantic::Position;
    if (compiler.has_decoration(id, spv::DecorationUserSemantic)) {
        const auto semantic = semantic_from_text(compiler.get_decoration_string(id, spv::DecorationUserSemantic), index);
        if (semantic != backend::TypedSemantic::None) return semantic;
    }
    return semantic_from_text(name, index);
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

std::string spirv_string(const uint32_t *words, size_t word_count) {
    std::string out;
    out.reserve(word_count * 4);
    for (size_t i = 0; i < word_count; ++i) {
        const uint32_t word = words[i];
        for (unsigned shift = 0; shift < 32; shift += 8) {
            const char c = static_cast<char>((word >> shift) & 0xffu);
            if (!c) return out;
            out.push_back(c);
        }
    }
    return {};
}

} // namespace
#endif

bool spirv_cross_to_typed_shader(const std::vector<uint32_t> &words,
                                 backend::TypedStage stage,
                                 const char *entrypoint,
                                 backend::TypedShader &typed,
                                 std::string &error) {
    typed = backend::TypedShader(stage);
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
        const spv::ExecutionModel model = stage == backend::TypedStage::Fragment ?
            spv::ExecutionModelFragment : spv::ExecutionModelVertex;
        compiler.set_entry_point(name, model);
        const uint32_t function_id = compiler.get_entry_point(name, model).self;
        const auto reflected = compiler.get_shader_resources();
        auto &program = typed.program();

        struct UniformMember {
            uint16_t resource = std::numeric_limits<uint16_t>::max();
            backend::TypedValue value{};
            bool matrix = false;
        };
        struct ExtractInfo {
            backend::TypedValue source{};
            uint32_t component = 0;
        };

        std::unordered_map<uint32_t, backend::TypedValue> values;
        std::unordered_map<uint32_t, std::vector<UniformMember>> uniform_blocks;
        std::unordered_map<uint32_t, UniformMember> access_chain_members;
        std::unordered_map<uint32_t, uint16_t> matrix_values;
        std::unordered_map<uint32_t, ExtractInfo> extracts;
        std::unordered_map<uint32_t, uint32_t> constants;
        std::unordered_set<uint32_t> float_ones;
        std::unordered_set<uint32_t> glsl450_imports;
        std::unordered_map<uint32_t, uint16_t> outputs;

        auto add_resource = [&](backend::TypedResourceKind kind, backend::TypedValue value,
                                backend::TypedType type, const std::string &resource_name,
                                uint16_t index, backend::TypedSemantic semantic,
                                uint8_t semantic_index, uint16_t &resource_id) -> bool {
            resource_id = typed.add_resource(kind, value, type, resource_name, index, semantic, semantic_index);
            if (resource_id == std::numeric_limits<uint16_t>::max()) {
                error = "Typed IR resource table overflow";
                return false;
            }
            return true;
        };

        for (const auto &resource : reflected.stage_inputs) {
            if (!compiler.has_decoration(resource.id, spv::DecorationLocation)) continue;
            const uint32_t location = compiler.get_decoration(resource.id, spv::DecorationLocation);
            if (location > std::numeric_limits<uint16_t>::max()) { error = "stage input location is too large"; return false; }
            const auto type = typed_type(compiler.get_type(resource.type_id));
            if (type == backend::TypedType::Invalid) { error = "SPIRV-Cross stage input has unsupported type"; return false; }
            const auto value = program.input(type, static_cast<uint16_t>(location));
            if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR input"; return false; }
            uint8_t semantic_index = 0;
            const auto semantic = reflected_semantic(compiler, resource.id, resource.name, semantic_index);
            uint16_t resource_id = 0;
            if (!add_resource(backend::TypedResourceKind::Input, value, type, resource.name,
                              static_cast<uint16_t>(location), semantic, semantic_index, resource_id)) return false;
            values[resource.id] = value;
        }

        for (const auto &resource : reflected.uniform_buffers) {
            const auto &struct_type = compiler.get_type(resource.base_type_id);
            if (struct_type.basetype != spirv_cross::SPIRType::Struct) {
                error = "SPIRV-Cross uniform buffer is not a struct";
                return false;
            }
            auto &members = uniform_blocks[resource.id];
            members.resize(struct_type.member_types.size());
            for (uint32_t member = 0; member < struct_type.member_types.size(); ++member) {
                const auto &member_type = compiler.get_type(struct_type.member_types[member]);
                const uint32_t byte_offset = compiler.type_struct_member_offset(struct_type, member);
                if ((byte_offset & 3u) || byte_offset / 4u > std::numeric_limits<uint16_t>::max()) {
                    error = "uniform member offset is outside the Typed IR resource range";
                    return false;
                }
                const uint16_t index = static_cast<uint16_t>(byte_offset / 4u);
                std::string member_name = compiler.get_member_name(resource.base_type_id, member);
                if (member_name.empty()) member_name = resource.name + ".member" + std::to_string(member);
                uint16_t resource_id = 0;
                if (is_f32_mat4(member_type)) {
                    if (!add_resource(backend::TypedResourceKind::Matrix4, {}, backend::TypedType::F32x4,
                                      member_name, index, backend::TypedSemantic::None, 0, resource_id)) return false;
                    members[member] = {resource_id, {}, true};
                    continue;
                }
                const auto type = typed_type(member_type);
                if (type == backend::TypedType::Invalid) { error = "uniform member type is unsupported by Typed IR"; return false; }
                const auto value = program.uniform(type, index);
                if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR uniform"; return false; }
                if (!add_resource(backend::TypedResourceKind::Uniform, value, type, member_name, index,
                                  backend::TypedSemantic::None, 0, resource_id)) return false;
                members[member] = {resource_id, value, false};
            }
        }

        for (const auto &resource : reflected.sampled_images) {
            uint32_t binding = 0;
            if (compiler.has_decoration(resource.id, spv::DecorationBinding))
                binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            if (binding > std::numeric_limits<uint16_t>::max()) { error = "sampler binding is too large"; return false; }
            const auto value = program.sampler(static_cast<uint16_t>(binding));
            if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR sampler"; return false; }
            uint16_t resource_id = 0;
            if (!add_resource(backend::TypedResourceKind::Sampler2D, value, backend::TypedType::Sampler2D,
                              resource.name, static_cast<uint16_t>(binding), backend::TypedSemantic::None, 0,
                              resource_id)) return false;
            values[resource.id] = value;
        }

        for (const auto &resource : reflected.stage_outputs) {
            if (!compiler.has_decoration(resource.id, spv::DecorationLocation)) continue;
            const uint32_t location = compiler.get_decoration(resource.id, spv::DecorationLocation);
            if (location > std::numeric_limits<uint16_t>::max()) { error = "stage output location is too large"; return false; }
            const auto type = typed_type(compiler.get_type(resource.type_id));
            if (type == backend::TypedType::Invalid) { error = "stage output type is unsupported by Typed IR"; return false; }
            uint8_t semantic_index = 0;
            const auto semantic = reflected_semantic(compiler, resource.id, resource.name, semantic_index);
            uint16_t resource_id = 0;
            if (!add_resource(backend::TypedResourceKind::Output, {}, type, resource.name,
                              static_cast<uint16_t>(location), semantic, semantic_index, resource_id)) return false;
            outputs[resource.id] = resource_id;
        }

        for (auto id : compiler.get_ir().ids_for_type[spirv_cross::SPIRVariable::type]) {
            if (compiler.get_storage_class(id) != spv::StorageClassOutput ||
                !compiler.has_decoration(id, spv::DecorationBuiltIn) ||
                compiler.get_decoration(id, spv::DecorationBuiltIn) != spv::BuiltInPosition ||
                outputs.count(id))
                continue;
            const auto type = typed_type(compiler.get_type_from_variable(id));
            if (type != backend::TypedType::F32x4) { error = "BuiltIn Position is not float4"; return false; }
            std::string output_name = compiler.get_name(id);
            if (output_name.empty()) output_name = "position";
            uint16_t resource_id = 0;
            if (!add_resource(backend::TypedResourceKind::Output, {}, type, output_name, 0,
                              backend::TypedSemantic::Position, 0, resource_id)) return false;
            outputs[id] = resource_id;
        }

        for (size_t offset = 5; offset < words.size();) {
            const uint32_t first = words[offset];
            const uint16_t count = static_cast<uint16_t>(first >> 16);
            const uint16_t op = static_cast<uint16_t>(first);
            if (!count || offset + count > words.size()) { error = "malformed SPIR-V instruction stream"; return false; }
            const uint32_t *args = words.data() + offset + 1;
            if (op == spv::OpExtInstImport && count >= 3) {
                if (spirv_string(args + 1, count - 2) == "GLSL.std.450")
                    glsl450_imports.insert(args[0]);
            } else if (op == spv::OpConstant && count >= 4) {
                constants[args[1]] = args[2];
                const auto &constant_type = compiler.get_type(args[0]);
                if (constant_type.basetype == spirv_cross::SPIRType::Float && constant_type.width == 32 &&
                    constant_type.vecsize == 1 && args[2] == 0x3f800000u)
                    float_ones.insert(args[1]);
            }
            offset += count;
        }

        bool in_function = false;
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

            if (op == spv::OpAccessChain && count >= 4) {
                const auto block = uniform_blocks.find(args[2]);
                const auto index_it = constants.find(args[3]);
                if (block != uniform_blocks.end() && index_it != constants.end()) {
                    const uint32_t member = index_it->second;
                    if (member >= block->second.size()) { error = "uniform access-chain member is out of range"; return false; }
                    access_chain_members[args[1]] = block->second[member];
                }
            } else if (op == spv::OpLoad && count >= 4) {
                if (auto it = values.find(args[2]); it != values.end()) {
                    values[args[1]] = it->second;
                } else if (auto it = access_chain_members.find(args[2]); it != access_chain_members.end()) {
                    if (it->second.matrix) matrix_values[args[1]] = it->second.resource;
                    else values[args[1]] = it->second.value;
                } else {
                    error = "Typed IR adapter could not resolve an OpLoad source";
                    return false;
                }
            } else if (op == spv::OpCompositeExtract && count == 5) {
                auto source = values.find(args[2]);
                if (source == values.end()) { error = "Typed IR adapter could not resolve composite extract source"; return false; }
                extracts[args[1]] = {source->second, args[3]};
            } else if (op == spv::OpVectorShuffle && count == 9) {
                const auto result_type = typed_type(compiler.get_type(args[0]));
                const auto source = values.find(args[2]);
                if (result_type != backend::TypedType::F32x4 || source == values.end() ||
                    source->second.type() != backend::TypedType::F32x4) {
                    error = "vector shuffle is outside the validated float4 subset";
                    return false;
                }
                const bool identity = args[4] == 0 && args[5] == 1 && args[6] == 2 && args[7] == 3;
                const bool splat_x = args[4] == 0 && args[5] == 0 && args[6] == 0 && args[7] == 0;
                if (identity) {
                    values[args[1]] = source->second;
                } else if (splat_x) {
                    const auto dst = program.make_value<backend::TypedType::F32x4>();
                    if (!program.emit<backend::TypedOpcode::FloatSplat>(0, dst, source->second)) {
                        error = "failed to emit Typed IR float4 X splat"; return false;
                    }
                    values[args[1]] = dst;
                } else {
                    error = "vector shuffle pattern is not independently validated";
                    return false;
                }
            } else if (op == spv::OpCompositeConstruct && count >= 4) {
                const auto result_type = typed_type(compiler.get_type(args[0]));
                if (result_type != backend::TypedType::F32x4) { error = "unsupported composite construct result type"; return false; }
                if (count == 7 && args[2] == args[3] && args[2] == args[4] && args[2] == args[5]) {
                    const auto scalar = values.find(args[2]);
                    if (scalar != values.end() && scalar->second.type() == backend::TypedType::F32) {
                        const auto dst = program.make_value<backend::TypedType::F32x4>();
                        if (!program.emit<backend::TypedOpcode::FloatSplat>(0, dst, scalar->second)) {
                            error = "failed to emit Typed IR scalar splat"; return false;
                        }
                        values[args[1]] = dst;
                        offset += count;
                        continue;
                    }
                }
                backend::TypedValue source{};
                uint8_t extracted = 0;
                bool valid = true;
                for (uint16_t i = 2; i < count - 1; ++i) {
                    const uint32_t operand = args[i];
                    if (float_ones.count(operand)) continue;
                    const auto ext = extracts.find(operand);
                    if (ext == extracts.end() || ext->second.component != extracted ||
                        (source.kind() != backend::TypedValueKind::None && source.bits != ext->second.source.bits)) {
                        valid = false;
                        break;
                    }
                    source = ext->second.source;
                    ++extracted;
                }
                const uint8_t source_components = backend::typed_component_count(source.type());
                if (!valid || source.kind() == backend::TypedValueKind::None ||
                    (source_components != 2 && source_components != 3) || extracted != source_components) {
                    error = "composite construct is not a homogeneous position expansion";
                    return false;
                }
                const auto dst = program.make_value<backend::TypedType::F32x4>();
                if (!program.emit<backend::TypedOpcode::ConstructPosition>(0, dst, source)) {
                    error = "failed to emit Typed IR position construct"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpMatrixTimesVector && count == 5) {
                const auto matrix = matrix_values.find(args[2]);
                const auto vector = values.find(args[3]);
                if (matrix == matrix_values.end() || vector == values.end()) {
                    error = "matrix-times-vector operands are unresolved"; return false;
                }
                const auto dst_type = typed_type(compiler.get_type(args[0]));
                if (dst_type != backend::TypedType::F32x4 || vector->second.type() != backend::TypedType::F32x4) {
                    error = "matrix-times-vector is outside the float4 subset"; return false;
                }
                const auto dst = program.make_value<backend::TypedType::F32x4>();
                if (!program.emit<backend::TypedOpcode::TransformPosition>(0, dst, vector->second, {}, matrix->second)) {
                    error = "failed to emit Typed IR position transform"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpImageSampleImplicitLod && count >= 5) {
                const auto sampled = values.find(args[2]);
                const auto coordinate = values.find(args[3]);
                if (sampled == values.end() || coordinate == values.end() ||
                    sampled->second.type() != backend::TypedType::Sampler2D ||
                    coordinate->second.type() != backend::TypedType::F32x2 ||
                    typed_type(compiler.get_type(args[0])) != backend::TypedType::F32x4) {
                    error = "sample2D operands are outside the current Typed IR subset"; return false;
                }
                const auto dst = program.make_value<backend::TypedType::F32x4>();
                if (!program.emit<backend::TypedOpcode::Sample2D>(0, dst, sampled->second, coordinate->second)) {
                    error = "failed to emit Typed IR sample2D"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpExtInst) {
                if (count < 6 || !glsl450_imports.count(args[2])) {
                    error = "unsupported extended instruction set";
                    return false;
                }
                const auto result_type = typed_type(compiler.get_type(args[0]));
                const uint32_t ext = args[3];
                if (ext == GLSLstd450FAbs) {
                    if (count != 6 || result_type != backend::TypedType::F32x4) {
                        error = "GLSL.std.450 FAbs is outside the validated float4 subset";
                        return false;
                    }
                    const auto source = values.find(args[4]);
                    if (source == values.end() || source->second.type() != result_type) {
                        error = "unresolved GLSL.std.450 FAbs operand";
                        return false;
                    }
                    const auto dst = program.make_value(result_type);
                    if (!program.emit<backend::TypedOpcode::FloatUnary>(
                            static_cast<uint8_t>(backend::TypedFloatUnaryOp::Abs), dst, source->second)) {
                        error = "failed to emit Typed IR FAbs"; return false;
                    }
                    values[args[1]] = dst;
                } else if (ext == GLSLstd450FMin || ext == GLSLstd450FMax) {
                    if (count != 7 || result_type != backend::TypedType::F32x4) {
                        error = "GLSL.std.450 min/max is outside the validated float4 subset";
                        return false;
                    }
                    const auto lhs = values.find(args[4]);
                    const auto rhs = values.find(args[5]);
                    if (lhs == values.end() || rhs == values.end() ||
                        lhs->second.type() != result_type || rhs->second.type() != result_type) {
                        error = "unresolved GLSL.std.450 min/max operand";
                        return false;
                    }
                    const auto dst = program.make_value(result_type);
                    const auto float_op = ext == GLSLstd450FMin ?
                        backend::TypedFloatOp::Min : backend::TypedFloatOp::Max;
                    if (!program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(float_op), dst, lhs->second, rhs->second)) {
                        error = "failed to emit Typed IR min/max"; return false;
                    }
                    values[args[1]] = dst;
                } else {
                    error = "GLSL.std.450 instruction is not in the validated Typed IR subset";
                    return false;
                }
            } else if (op == spv::OpFConvert) {
                if (count != 4) { error = "invalid floating conversion instruction"; return false; }
                const auto source = values.find(args[2]);
                const auto result_type = typed_type(compiler.get_type(args[0]));
                if (source == values.end() || source->second.type() != backend::TypedType::F32x4 ||
                    result_type != backend::TypedType::F16x4) {
                    error = "floating conversion is outside the validated F32x4 to F16x4 subset";
                    return false;
                }
                const auto dst = program.make_value<backend::TypedType::F16x4>();
                if (!program.emit<backend::TypedOpcode::FloatConvert>(
                        static_cast<uint8_t>(backend::TypedFloatConvertOp::F32x4ToF16x4),
                        dst, source->second)) {
                    error = "failed to emit Typed IR float conversion"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpFNegate) {
                if (count != 4) { error = "invalid floating unary instruction"; return false; }
                const auto source = values.find(args[2]);
                const auto result_type = typed_type(compiler.get_type(args[0]));
                if (source == values.end() || result_type == backend::TypedType::Invalid ||
                    source->second.type() != result_type || !backend::typed_is_float(result_type)) {
                    error = "floating unary operand is unresolved or mismatched"; return false;
                }
                const auto dst = program.make_value(result_type);
                if (!program.emit<backend::TypedOpcode::FloatUnary>(
                        static_cast<uint8_t>(backend::TypedFloatUnaryOp::Neg), dst, source->second)) {
                    error = "failed to emit Typed IR float unary operation"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpFMul || op == spv::OpFAdd || op == spv::OpFSub) {
                if (count != 5) { error = "invalid floating binary instruction"; return false; }
                const auto lhs = values.find(args[2]);
                const auto rhs = values.find(args[3]);
                const auto result_type = typed_type(compiler.get_type(args[0]));
                if (lhs == values.end() || rhs == values.end() || result_type == backend::TypedType::Invalid ||
                    lhs->second.type() != result_type || rhs->second.type() != result_type || !backend::typed_is_float(result_type)) {
                    error = "floating binary operands are unresolved or mismatched"; return false;
                }
                backend::TypedFloatOp float_op = backend::TypedFloatOp::Mul;
                if (op == spv::OpFAdd) float_op = backend::TypedFloatOp::Add;
                else if (op == spv::OpFSub) float_op = backend::TypedFloatOp::Sub;
                const auto dst = program.make_value(result_type);
                if (!program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(float_op), dst,
                                                                     lhs->second, rhs->second)) {
                    error = "failed to emit Typed IR float binary operation"; return false;
                }
                values[args[1]] = dst;
            } else if (op == spv::OpStore && count >= 3) {
                const auto output = outputs.find(args[0]);
                const auto value = values.find(args[1]);
                if (output == outputs.end() || value == values.end()) {
                    error = "Typed IR adapter encountered a non-output store or unresolved value"; return false;
                }
                if (!program.emit<backend::TypedOpcode::StoreOutput>(0, {}, value->second, {}, output->second)) {
                    error = "failed to emit Typed IR output store"; return false;
                }
            } else if (op != spv::OpLabel && op != spv::OpReturn && op != spv::OpNop) {
                error = "unsupported instruction in SPIRV-Cross Typed shader subset";
                return false;
            }
            offset += count;
        }

        return true;
    } catch (const std::exception &e) {
        error = std::string("SPIRV-Cross Typed shader adapter: ") + e.what();
        return false;
    }
#endif
}

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
