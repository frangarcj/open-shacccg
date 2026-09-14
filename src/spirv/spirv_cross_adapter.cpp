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

bool is_scalar_f32(const spirv_cross::Compiler &compiler, uint32_t type_id) {
    const auto &type = compiler.get_type(type_id);
    return type.basetype == spirv_cross::SPIRType::Float && type.width == 32 &&
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
    if (type.basetype == spirv_cross::SPIRType::Int && type.width == 32)
        return vector_type(backend::TypedType::S32, backend::TypedType::U32x2,
                           backend::TypedType::U32x3, backend::TypedType::U32x4);
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

bool map_float_compare(uint16_t op, usse::CompareOp &mapped) {
    switch (static_cast<spv::Op>(op)) {
    case spv::OpFOrdEqual: mapped = usse::CompareOp::Equal; return true;
    case spv::OpFOrdNotEqual: mapped = usse::CompareOp::NotEqual; return true;
    // glslang's HLSL frontend emits the Cg/HLSL `!=` operator as unordered
    // not-equal. The Sony oracle probe fp-cmp-ne-big anchors this source-level
    // operation to the same F32 VTST not-equal profile used by Typed IR.
    case spv::OpFUnordNotEqual: mapped = usse::CompareOp::NotEqual; return true;
    case spv::OpFOrdLessThan: mapped = usse::CompareOp::Less; return true;
    case spv::OpFOrdLessThanEqual: mapped = usse::CompareOp::LessEqual; return true;
    case spv::OpFOrdGreaterThan: mapped = usse::CompareOp::Greater; return true;
    case spv::OpFOrdGreaterThanEqual: mapped = usse::CompareOp::GreaterEqual; return true;
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
        struct PhiSink {
            uint32_t merge_label = 0;
            uint32_t value0 = 0;
            uint32_t label0 = 0;
            uint32_t value1 = 0;
            uint32_t label1 = 0;
            uint16_t output_resource = std::numeric_limits<uint16_t>::max();
            backend::TypedType type = backend::TypedType::Invalid;
            uint32_t continue_label = 0;
            bool loop_state = false;
        };
        struct SelectSink {
            uint32_t condition = 0;
            uint32_t true_value = 0;
            uint32_t false_value = 0;
            uint16_t output_resource = std::numeric_limits<uint16_t>::max();
        };

        std::unordered_map<uint32_t, backend::TypedValue> values;
        std::unordered_map<uint32_t, std::vector<UniformMember>> uniform_blocks;
        std::unordered_map<uint32_t, UniformMember> access_chain_members;
        std::unordered_map<uint32_t, ExtractInfo> input_access_chains;
        std::unordered_map<uint32_t, uint16_t> matrix_values;
        std::unordered_map<uint32_t, ExtractInfo> extracts;
        std::unordered_map<uint32_t, uint32_t> constants;
        std::unordered_map<uint32_t, std::array<uint32_t,4>> float_vector_constants;
        std::unordered_set<uint32_t> float_ones;
        std::unordered_set<uint32_t> glsl450_imports;
        std::unordered_map<uint32_t, uint16_t> outputs;
        std::unordered_map<uint32_t, PhiSink> phi_sinks;
        std::unordered_map<uint32_t, SelectSink> select_sinks;
        std::unordered_map<uint32_t, uint16_t> block_labels;
        std::unordered_map<uint32_t, uint32_t> loop_headers;
        std::unordered_set<uint32_t> s32_loop_state_ids;

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
            const auto value = type==backend::TypedType::F32 ?
                program.input_component_f32(static_cast<uint16_t>(location/2u),static_cast<uint8_t>(location&1u)) :
                program.input(type, static_cast<uint16_t>(location));
            if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR input"; return false; }
            uint8_t semantic_index = 0;
            auto semantic = reflected_semantic(compiler, resource.id, resource.name, semantic_index);
            // Glslang's HLSL path used by the Cg compatibility frontend can
            // lower a return value declared `: POSITION` to Location 0 without
            // retaining UserSemantic/BuiltIn decorations. A Vita vertex must
            // have a position output; in this no-BuiltIn form Location 0 is the
            // oracle-backed position slot for the differential vertex probes.
            if (stage==backend::TypedStage::Vertex && semantic==backend::TypedSemantic::None && location==0)
                semantic=backend::TypedSemantic::Position;
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
            } else if (op==spv::OpConstantComposite) {
                const auto constant_type=typed_type(compiler.get_type(args[0]));
                const uint8_t components=backend::typed_component_count(constant_type);
                const bool supported=constant_type==backend::TypedType::F32x2 ||
                    constant_type==backend::TypedType::F32x3 || constant_type==backend::TypedType::F32x4;
                if (!supported || count!=static_cast<uint16_t>(components+3u)) {
                    offset += count;
                    continue;
                }
                std::array<uint32_t,4> bits{};
                bool resolved=true;
                for (uint8_t lane=0;lane<components;++lane) {
                    auto scalar=constants.find(args[2+lane]);
                    if (scalar==constants.end()) { resolved=false; break; }
                    bits[lane]=scalar->second;
                }
                if (resolved) float_vector_constants[args[1]]=bits;
            }
            offset += count;
        }

        // Pre-scan structured control flow so two-way phis that feed the final
        // fragment output can be sunk into their predecessor blocks. This keeps
        // Typed/Machine IR SSA without inventing multi-definition virtual values.
        if (stage == backend::TypedStage::Fragment) {
            bool scan_function=false;
            uint32_t scan_label=0;
            std::vector<uint32_t> function_labels;
            bool has_control=false;
            for (size_t offset=5; offset<words.size();) {
                const uint32_t first=words[offset];
                const uint16_t count=static_cast<uint16_t>(first>>16);
                const uint16_t op=static_cast<uint16_t>(first);
                if (!count || offset+count>words.size()) { error="malformed SPIR-V control-flow stream"; return false; }
                const uint32_t *args=words.data()+offset+1;
                if (op==spv::OpFunction && count>=3) {
                    scan_function=args[1]==function_id;
                } else if (scan_function && op==spv::OpFunctionEnd) {
                    scan_function=false;
                    scan_label=0;
                } else if (scan_function && op==spv::OpLabel && count==2) {
                    scan_label=args[0];
                    function_labels.push_back(scan_label);
                } else if (scan_function && (op==spv::OpBranch || op==spv::OpBranchConditional || op==spv::OpSwitch)) {
                    has_control=true;
                } else if (scan_function && op==spv::OpPhi) {
                    if (count!=7 || !scan_label) {
                        error="only two-way structured OpPhi is supported";
                        return false;
                    }
                    PhiSink phi{};
                    phi.merge_label=scan_label;
                    phi.value0=args[2]; phi.label0=args[3];
                    phi.value1=args[4]; phi.label1=args[5];
                    phi.type=typed_type(compiler.get_type(args[0]));
                    if (phi.type==backend::TypedType::Invalid) {
                        error="OpPhi type is outside the validated Typed shader subset";
                        return false;
                    }
                    phi_sinks[args[1]]=phi;
                } else if (scan_function && op==spv::OpLoopMerge) {
                    if (count!=4 || !scan_label) {
                        error="invalid structured OpLoopMerge";
                        return false;
                    }
                    loop_headers[scan_label]=args[1];
                    has_control=true;
                } else if (scan_function && op==spv::OpSelect) {
                    if (count!=6) {
                        error="only ordinary three-operand OpSelect is supported";
                        return false;
                    }
                    select_sinks[args[1]]={args[2],args[3],args[4],std::numeric_limits<uint16_t>::max()};
                    has_control=true;
                } else if (scan_function && op==spv::OpStore && count>=3) {
                    const auto phi=phi_sinks.find(args[1]);
                    const auto output=outputs.find(args[0]);
                    if (phi!=phi_sinks.end() && output!=outputs.end())
                        phi->second.output_resource=output->second;
                    const auto select=select_sinks.find(args[1]);
                    if (select!=select_sinks.end() && output!=outputs.end())
                        select->second.output_resource=output->second;
                }
                offset+=count;
            }
            for (auto &entry:phi_sinks) {
                auto loop=loop_headers.find(entry.second.merge_label);
                if (loop!=loop_headers.end()) {
                    entry.second.loop_state=true;
                    entry.second.continue_label=loop->second;
                }
            }
            if (has_control) {
                for (uint32_t id:function_labels) {
                    const uint16_t label=program.make_label();
                    if (label==std::numeric_limits<uint16_t>::max()) {
                        error="Typed shader control-flow label table overflow";
                        return false;
                    }
                    block_labels[id]=label;
                }
            }
        }

        bool in_function = false;
        uint32_t current_label = 0;
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

            if (op == spv::OpLabel && count == 2) {
                current_label=args[0];
                if (!block_labels.empty()) {
                    const auto label=block_labels.find(current_label);
                    if (label==block_labels.end() || !program.bind_label(label->second)) {
                        error="failed to bind Typed shader control-flow label";
                        return false;
                    }
                }
            } else if (op == spv::OpAccessChain && count >= 4) {
                const auto block = uniform_blocks.find(args[2]);
                const auto index_it = constants.find(args[3]);
                if (block != uniform_blocks.end() && index_it != constants.end()) {
                    const uint32_t member = index_it->second;
                    if (member >= block->second.size()) { error = "uniform access-chain member is out of range"; return false; }
                    access_chain_members[args[1]] = block->second[member];
                } else if (index_it != constants.end()) {
                    const auto input=values.find(args[2]);
                    if (input!=values.end() && typed_is_float(input->second.type())) {
                        const uint32_t component=index_it->second;
                        if (component>=backend::typed_component_count(input->second.type())) {
                            error="input access-chain component is out of range";
                            return false;
                        }
                        input_access_chains[args[1]]={input->second,component};
                    }
                }
            } else if (op == spv::OpLoad && count >= 4) {
                if (auto it = values.find(args[2]); it != values.end()) {
                    values[args[1]] = it->second;
                } else if (auto it = access_chain_members.find(args[2]); it != access_chain_members.end()) {
                    if (it->second.matrix) matrix_values[args[1]] = it->second.resource;
                    else values[args[1]] = it->second.value;
                } else if (auto it=input_access_chains.find(args[2]); it!=input_access_chains.end()) {
                    if (it->second.component!=0 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::F32) {
                        error="only float-vector X access is validated for control-flow comparisons";
                        return false;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (!program.emit<backend::TypedOpcode::FloatExtract>(0,dst,it->second.source)) {
                        error="failed to emit Typed IR float X extraction";
                        return false;
                    }
                    values[args[1]]=dst;
                } else {
                    error = "Typed IR adapter could not resolve an OpLoad source";
                    return false;
                }
            } else if (op == spv::OpCompositeExtract && count == 5) {
                auto source = values.find(args[2]);
                if (source == values.end()) { error = "Typed IR adapter could not resolve composite extract source"; return false; }
                extracts[args[1]] = {source->second, args[3]};
                const auto result_type=typed_type(compiler.get_type(args[0]));
                const uint8_t components=backend::typed_component_count(source->second.type());
                if (result_type==backend::TypedType::F32 && backend::typed_is_float(source->second.type()) &&
                    components>=2 && components<=4 && args[3]<components) {
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatExtract>(static_cast<uint8_t>(args[3]),dst,source->second)) {
                        error="failed to emit Typed float component extraction";
                        return false;
                    }
                    values[args[1]]=dst;
                }
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
                const bool wzyx = args[4] == 3 && args[5] == 2 && args[6] == 1 && args[7] == 0;
                if (identity) {
                    values[args[1]] = source->second;
                } else if (splat_x) {
                    const auto dst = program.make_value<backend::TypedType::F32x4>();
                    if (!program.emit<backend::TypedOpcode::FloatSplat>(0, dst, source->second)) {
                        error = "failed to emit Typed IR float4 X splat"; return false;
                    }
                    values[args[1]] = dst;
                } else if (wzyx) {
                    const auto dst=program.make_value<backend::TypedType::F32x4>();
                    if (!program.emit<backend::TypedOpcode::FloatSwizzle>(
                            static_cast<uint8_t>(backend::TypedFloatSwizzleOp::Wzyx),dst,source->second)) {
                        error="failed to emit oracle-validated wzyx Typed swizzle";
                        return false;
                    }
                    values[args[1]]=dst;
                } else {
                    error = "vector shuffle pattern is not independently validated";
                    return false;
                }
            } else if (op == spv::OpCompositeConstruct && count >= 4) {
                const auto &spirv_result_type=compiler.get_type(args[0]);
                if (spirv_result_type.basetype==spirv_cross::SPIRType::Boolean &&
                    spirv_result_type.vecsize==4 && count==7 &&
                    args[2]==args[3] && args[2]==args[4] && args[2]==args[5]) {
                    const auto predicate=values.find(args[2]);
                    if (predicate==values.end() || predicate->second.kind()!=backend::TypedValueKind::Predicate) {
                        error="boolean vector splat is not sourced by a Typed predicate";
                        return false;
                    }
                    values[args[1]]=predicate->second;
                    offset+=count;
                    continue;
                }
                const auto result_type = typed_type(spirv_result_type);
                const uint8_t result_components=backend::typed_component_count(result_type);
                const bool splat_vector=result_type==backend::TypedType::F32x2 ||
                    result_type==backend::TypedType::F32x3 || result_type==backend::TypedType::F32x4;
                bool repeated=splat_vector && count==static_cast<uint16_t>(result_components+3u);
                if (repeated) {
                    for (uint8_t lane=1;lane<result_components;++lane)
                        repeated = repeated && args[2+lane]==args[2];
                }
                if (repeated) {
                    const auto scalar = values.find(args[2]);
                    if (scalar != values.end() && scalar->second.type() == backend::TypedType::F32) {
                        const auto dst = program.make_value(result_type);
                        if (!program.emit<backend::TypedOpcode::FloatSplat>(0, dst, scalar->second)) {
                            error = "failed to emit Typed IR scalar splat"; return false;
                        }
                        values[args[1]] = dst;
                        offset += count;
                        continue;
                    }
                }
                if (result_type != backend::TypedType::F32x4) { error = "unsupported composite construct result type"; return false; }
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
            } else if ((op == spv::OpMatrixTimesVector || op == spv::OpVectorTimesMatrix) && count == 5) {
                const uint32_t matrix_id = op == spv::OpMatrixTimesVector ? args[2] : args[3];
                const uint32_t vector_id = op == spv::OpMatrixTimesVector ? args[3] : args[2];
                const auto matrix = matrix_values.find(matrix_id);
                const auto vector = values.find(vector_id);
                if (matrix == matrix_values.end() || vector == values.end()) {
                    error = "matrix/vector multiply operands are unresolved"; return false;
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
                const uint8_t result_components=backend::typed_component_count(result_type);
                const bool result_scalar=result_type==backend::TypedType::F32;
                const bool result_vector=result_type==backend::TypedType::F32x2 ||
                    result_type==backend::TypedType::F32x3 || result_type==backend::TypedType::F32x4;
                const bool result_float=result_scalar || result_vector;
                if (ext == GLSLstd450FAbs) {
                    if (count != 6 || !result_float) {
                        error = "GLSL.std.450 FAbs is outside the validated F32 subset";
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
                    if (count != 7 || !result_float) {
                        error = "GLSL.std.450 min/max is outside the validated F32 subset";
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
                } else if (ext == GLSLstd450FClamp) {
                    if (count != 8 || !result_float) {
                        error="GLSL.std.450 FClamp is outside the validated F32 subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    auto constant_bits=[&](uint32_t id, uint32_t expected) {
                        if (result_scalar) {
                            const auto it=constants.find(id);
                            return it!=constants.end() && it->second==expected;
                        }
                        const auto it=float_vector_constants.find(id);
                        if (it==float_vector_constants.end()) return false;
                        for (uint8_t lane=0;lane<result_components;++lane)
                            if (it->second[lane]!=expected) return false;
                        return true;
                    };
                    if (source==values.end() || source->second.type()!=result_type ||
                        !constant_bits(args[5],0u) || !constant_bits(args[6],0x3f800000u)) {
                        error="FClamp bounds are not the validated F32 zero/one constants";
                        return false;
                    }
                    const auto dst=program.make_value(result_type);
                    if (!program.emit<backend::TypedOpcode::FloatUnary>(
                            static_cast<uint8_t>(backend::TypedFloatUnaryOp::Saturate),dst,source->second)) {
                        error="failed to emit Typed IR saturate";
                        return false;
                    }
                    values[args[1]]=dst;
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
            } else if (op == spv::OpConvertFToS) {
                if (count!=4 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::S32) {
                    error="float-to-signed conversion is outside the validated scalar S32 subset";
                    return false;
                }
                const auto source=values.find(args[2]);
                if (source==values.end() || source->second.type()!=backend::TypedType::F32) {
                    error="F32->S32 conversion source is unresolved or non-F32";
                    return false;
                }
                const auto dst=program.make_value<backend::TypedType::S32>();
                if (dst.kind()==backend::TypedValueKind::None ||
                    !program.emit<backend::TypedOpcode::FloatToS32>(0,dst,source->second)) {
                    error="failed to emit Typed F32->S32 conversion";
                    return false;
                }
                values[args[1]]=dst;
            } else if (op == spv::OpConvertSToF) {
                if (count!=4 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::F32) {
                    error="signed-to-float conversion is outside the validated scalar F32 subset";
                    return false;
                }
                const auto source=values.find(args[2]);
                if (source==values.end() || source->second.type()!=backend::TypedType::S32) {
                    error="S32->F32 conversion source is unresolved or non-S32";
                    return false;
                }
                const auto dst=program.make_value<backend::TypedType::F32>();
                if (dst.kind()==backend::TypedValueKind::None ||
                    !program.emit<backend::TypedOpcode::S32ToFloat>(0,dst,source->second)) {
                    error="failed to emit Typed S32->F32 conversion";
                    return false;
                }
                values[args[1]]=dst;
            } else {
                usse::BitwiseOp bitwise{};
                usse::CompareOp compare{};
                if (map_bitwise(op,bitwise)) {
                    if (count!=5) { error="invalid scalar integer bitwise instruction"; return false; }
                    const auto result_type=typed_type(compiler.get_type(args[0]));
                    const bool scalar=result_type==backend::TypedType::S32 || result_type==backend::TypedType::U32;
                    const bool int2=result_type==backend::TypedType::U32x2 && bitwise==usse::BitwiseOp::Or;
                    if (!scalar && !int2) {
                        error="bitwise result is outside the validated scalar or int2-OR subset";
                        return false;
                    }
                    auto resolve_integer=[&](uint32_t id, backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id); it!=values.end() && it->second.type()==result_type) {
                            value=it->second;
                            return true;
                        }
                        if (int2) return false;
                        const auto constant=constants.find(id);
                        if (constant==constants.end()) return false;
                        value=result_type==backend::TypedType::S32 ?
                            program.literal_s32(static_cast<int32_t>(constant->second)) :
                            program.literal_u32(constant->second);
                        return value.kind()!=backend::TypedValueKind::None;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (!resolve_integer(args[2],lhs) || !resolve_integer(args[3],rhs)) {
                        error="integer bitwise operands are unresolved or mismatched";
                        return false;
                    }
                    const auto dst=program.make_value(result_type);
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::Bitwise>(static_cast<uint8_t>(bitwise),dst,lhs,rhs)) {
                        error="failed to emit Typed integer bitwise operation";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op==spv::OpSLessThan) {
                    if (count!=5) { error="invalid signed integer compare instruction"; return false; }
                    const auto lhs=values.find(args[2]);
                    const auto rhs=values.find(args[3]);
                    if (lhs==values.end() || rhs==values.end() ||
                        lhs->second.type()!=backend::TypedType::S32 || rhs->second.type()!=backend::TypedType::S32) {
                        error="signed loop compare operands are unresolved or non-S32";
                        return false;
                    }
                    const auto dst=program.make_predicate();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::Compare>(
                            static_cast<uint8_t>(usse::CompareOp::Less),dst,lhs->second,rhs->second)) {
                        error="failed to emit Typed S32 loop compare";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op==spv::OpIAdd) {
                    if (count!=5 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::S32) {
                        error="integer add is outside the validated S32 loop subset";
                        return false;
                    }
                    backend::TypedValue state{};
                    uint32_t constant_id=0;
                    auto lhs=values.find(args[2]);
                    auto rhs=values.find(args[3]);
                    if (lhs!=values.end() && lhs->second.type()==backend::TypedType::S32 &&
                        s32_loop_state_ids.count(lhs->second.id())) {
                        state=lhs->second;
                        constant_id=args[3];
                    } else if (rhs!=values.end() && rhs->second.type()==backend::TypedType::S32 &&
                               s32_loop_state_ids.count(rhs->second.id())) {
                        state=rhs->second;
                        constant_id=args[2];
                    }
                    const auto constant=constants.find(constant_id);
                    const uint32_t step=constant==constants.end()?0:constant->second;
                    if (state.kind()==backend::TypedValueKind::None || step<1 || step>3 ||
                        !program.emit<backend::TypedOpcode::IntIncrement>(static_cast<uint8_t>(step),state)) {
                        error="S32 loop increment is outside oracle-validated step 1..3";
                        return false;
                    }
                    values[args[1]]=state;
                } else if (map_float_compare(op,compare)) {
                    if (count!=5) { error="invalid floating compare instruction"; return false; }
                    auto resolve_scalar_f32 = [&](uint32_t id, backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id); it!=values.end() && it->second.type()==backend::TypedType::F32) {
                            value=it->second;
                            return true;
                        }
                        const auto ext=extracts.find(id);
                        if (ext==extracts.end() || ext->second.component!=0 ||
                            !backend::typed_is_float(ext->second.source.type()) ||
                            backend::typed_component_count(ext->second.source.type())<2)
                            return false;
                        const auto scalar=program.make_value<backend::TypedType::F32>();
                        if (!program.emit<backend::TypedOpcode::FloatExtract>(0,scalar,ext->second.source)) return false;
                        values[id]=scalar;
                        value=scalar;
                        return true;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (!resolve_scalar_f32(args[2],lhs) || !resolve_scalar_f32(args[3],rhs)) {
                        error="floating compare operands are unresolved or outside scalar F32 subset";
                        return false;
                    }
                    const auto dst=program.make_predicate();
                    if (!program.emit<backend::TypedOpcode::Compare>(static_cast<uint8_t>(compare),dst,lhs,rhs)) {
                        error="failed to emit Typed IR F32 compare";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op == spv::OpDot) {
                    if (count!=5 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::F32) {
                        error="OpDot result is outside the validated scalar F32 subset";
                        return false;
                    }
                    const auto lhs=values.find(args[2]);
                    const auto rhs=values.find(args[3]);
                    if (lhs==values.end() || rhs==values.end() || lhs->second.type()!=rhs->second.type() ||
                        (lhs->second.type()!=backend::TypedType::F32x2 &&
                         lhs->second.type()!=backend::TypedType::F32x3 &&
                         lhs->second.type()!=backend::TypedType::F32x4)) {
                        error="OpDot operands are outside the validated F32 vector subset";
                        return false;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (!program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Dot),dst,lhs->second,rhs->second)) {
                        error="failed to emit Typed IR dot";
                        return false;
                    }
                    values[args[1]]=dst;
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
                } else if (op == spv::OpFMul || op == spv::OpFAdd || op == spv::OpFSub || op == spv::OpFDiv) {
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
                    else if (op == spv::OpFDiv) float_op = backend::TypedFloatOp::Div;
                    const auto dst = program.make_value(result_type);
                    if (!program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(float_op), dst,
                                                                         lhs->second, rhs->second)) {
                        error = "failed to emit Typed IR float binary operation"; return false;
                    }
                    values[args[1]] = dst;
                } else if (op == spv::OpSelectionMerge) {
                    // Structured merge metadata is consumed by the control pre-pass.
                } else if (op == spv::OpBranchConditional) {
                    if (count!=4 || block_labels.empty()) { error="conditional branch is outside structured Typed shader subset"; return false; }
                    const auto predicate=values.find(args[0]);
                    const auto true_label=block_labels.find(args[1]);
                    const auto false_label=block_labels.find(args[2]);
                    if (predicate==values.end() || predicate->second.kind()!=backend::TypedValueKind::Predicate ||
                        true_label==block_labels.end() || false_label==block_labels.end() ||
                        !program.branch(true_label->second,predicate->second) || !program.jump(false_label->second)) {
                        error="failed to emit Typed shader conditional control flow";
                        return false;
                    }
                } else if (op == spv::OpBranch) {
                    if (count!=2 || block_labels.empty()) { error="unconditional branch is outside structured Typed shader subset"; return false; }
                    const uint32_t target_id=args[0];
                    // Loop phis become explicit mutable state. Initialize that
                    // state on the entry edge and update it on the continue
                    // back-edge; the header itself then only aliases the phi id
                    // to the stable Typed state handle.
                    for (auto &entry:phi_sinks) {
                        auto &phi=entry.second;
                        if (!phi.loop_state || phi.merge_label!=target_id) continue;
                        uint32_t incoming=0;
                        if (phi.label0==current_label) incoming=phi.value0;
                        else if (phi.label1==current_label) incoming=phi.value1;
                        else {
                            error="loop phi has no incoming value for predecessor";
                            return false;
                        }
                        if (current_label!=phi.continue_label) {
                            backend::TypedValue initial{};
                            if (auto value=values.find(incoming); value!=values.end()) {
                                initial=value->second;
                            } else if (phi.type==backend::TypedType::S32) {
                                auto constant=constants.find(incoming);
                                if (constant==constants.end()) {
                                    error="S32 loop phi initial value is not a constant";
                                    return false;
                                }
                                initial=program.literal_s32(static_cast<int32_t>(constant->second));
                            }
                            if (initial.kind()==backend::TypedValueKind::None || initial.type()!=phi.type) {
                                error="loop phi initial value is unresolved or type-mismatched";
                                return false;
                            }
                            const auto state=program.make_value(phi.type);
                            if (state.kind()==backend::TypedValueKind::None ||
                                !program.emit<backend::TypedOpcode::StateInit>(0,state,initial)) {
                                error="failed to initialize Typed loop state";
                                return false;
                            }
                            values[entry.first]=state;
                            if (phi.type==backend::TypedType::S32) s32_loop_state_ids.insert(state.id());
                        } else {
                            const auto state=values.find(entry.first);
                            const auto next=values.find(incoming);
                            if (state==values.end() || next==values.end()) {
                                error="loop back-edge state is unresolved";
                                return false;
                            }
                            if (phi.type==backend::TypedType::F32x4) {
                                if (next->second.type()!=phi.type ||
                                    !program.emit<backend::TypedOpcode::StateUpdate>(0,state->second,next->second)) {
                                    error="failed to update Typed float4 loop state";
                                    return false;
                                }
                            } else if (phi.type==backend::TypedType::S32) {
                                if (next->second.bits!=state->second.bits) {
                                    error="S32 loop back-edge is not the validated in-place increment";
                                    return false;
                                }
                            } else {
                                error="loop state type is outside the validated float4/S32 subset";
                                return false;
                            }
                        }
                    }
                    // If this predecessor contributes to a two-way phi that feeds
                    // fragment COLOR0, sink the store before leaving the block.
                    for (const auto &entry:phi_sinks) {
                        const auto &phi=entry.second;
                        if (phi.loop_state) continue;
                        if (phi.merge_label!=target_id || phi.output_resource==std::numeric_limits<uint16_t>::max()) continue;
                        uint32_t incoming=0;
                        if (phi.label0==current_label) incoming=phi.value0;
                        else if (phi.label1==current_label) incoming=phi.value1;
                        else continue;
                        const auto value=values.find(incoming);
                        if (value==values.end() || value->second.type()!=backend::TypedType::F32x4 ||
                            !program.emit<backend::TypedOpcode::StoreOutput>(0,{},value->second,{},phi.output_resource)) {
                            error="failed to sink phi-fed fragment output into predecessor";
                            return false;
                        }
                    }
                    const auto target=block_labels.find(target_id);
                    if (target==block_labels.end() || !program.jump(target->second)) {
                        error="failed to emit Typed shader jump";
                        return false;
                    }
                } else if (op == spv::OpPhi) {
                    const auto phi=phi_sinks.find(args[1]);
                    if (phi==phi_sinks.end()) {
                        error="OpPhi is outside the validated structured subset";
                        return false;
                    }
                    if (phi->second.loop_state) {
                        if (values.find(args[1])==values.end()) {
                            error="loop phi state was not initialized on its entry edge";
                            return false;
                        }
                    } else if (phi->second.output_resource==std::numeric_limits<uint16_t>::max()) {
                        error="OpPhi is not the validated two-way fragment-output merge";
                        return false;
                    }
                    // The corresponding stores were sunk into the predecessor blocks.
                } else if (op == spv::OpSelect) {
                    const auto select=select_sinks.find(args[1]);
                    if (select==select_sinks.end() || select->second.output_resource==std::numeric_limits<uint16_t>::max()) {
                        error="OpSelect is not a validated fragment-output selection";
                        return false;
                    }
                    const auto predicate=values.find(select->second.condition);
                    const auto true_value=values.find(select->second.true_value);
                    const auto false_value=values.find(select->second.false_value);
                    if (predicate==values.end() || predicate->second.kind()!=backend::TypedValueKind::Predicate ||
                        true_value==values.end() || false_value==values.end() ||
                        true_value->second.type()!=backend::TypedType::F32x4 ||
                        false_value->second.type()!=backend::TypedType::F32x4) {
                        error="OpSelect operands are outside the validated float4 fragment-output subset";
                        return false;
                    }
                    const uint16_t true_label=program.make_label();
                    const uint16_t merge_label=program.make_label();
                    if (true_label==std::numeric_limits<uint16_t>::max() ||
                        merge_label==std::numeric_limits<uint16_t>::max() ||
                        !program.branch(true_label,predicate->second) ||
                        !program.emit<backend::TypedOpcode::StoreOutput>(0,{},false_value->second,{},select->second.output_resource) ||
                        !program.jump(merge_label) || !program.bind_label(true_label) ||
                        !program.emit<backend::TypedOpcode::StoreOutput>(0,{},true_value->second,{},select->second.output_resource) ||
                        !program.bind_label(merge_label)) {
                        error="failed to lower fragment OpSelect to validated Typed control flow";
                        return false;
                    }
                } else if (op == spv::OpStore && count >= 3) {
                    const auto phi=phi_sinks.find(args[1]);
                    const auto select=select_sinks.find(args[1]);
                    if ((phi!=phi_sinks.end() && !phi->second.loop_state && phi->second.output_resource!=std::numeric_limits<uint16_t>::max()) ||
                        (select!=select_sinks.end() && select->second.output_resource!=std::numeric_limits<uint16_t>::max())) {
                        // Already materialized in each predecessor.
                    } else {
                        const auto output = outputs.find(args[0]);
                        auto value = values.find(args[1]);
                        if (value==values.end()) {
                            const auto constant=float_vector_constants.find(args[1]);
                            if (constant!=float_vector_constants.end() && output!=outputs.end() &&
                                output->second<typed.resources().size() &&
                                typed.resources()[output->second].type==backend::TypedType::F32x4) {
                                const auto literal=program.literal_f32x4(constant->second);
                                if (literal.kind()==backend::TypedValueKind::None) {
                                    error="failed to materialize fragment float4 output constant";
                                    return false;
                                }
                                values[args[1]]=literal;
                                value=values.find(args[1]);
                            }
                        }
                        if (output == outputs.end() || value == values.end()) {
                            error = "Typed IR adapter encountered a non-output store or unresolved value"; return false;
                        }
                        if (!program.emit<backend::TypedOpcode::StoreOutput>(0, {}, value->second, {}, output->second)) {
                            error = "failed to emit Typed IR output store"; return false;
                        }
                    }
                } else if (op == spv::OpLoopMerge) {
                    // Structured loop metadata was consumed by the pre-scan.
                } else if (op == spv::OpSwitch) {
                    // glslang wraps early-return HLSL/Cg functions in a
                    // selection containing an OpSwitch with no case pairs.
                    // With only selector + default operands this is exactly an
                    // unconditional jump regardless of selector value.
                    if (count!=3 || block_labels.empty()) {
                        error="only case-free OpSwitch is validated for Typed shader control flow";
                        return false;
                    }
                    const auto target=block_labels.find(args[1]);
                    if (target==block_labels.end() || !program.jump(target->second)) {
                        error="failed to emit case-free OpSwitch as Typed jump";
                        return false;
                    }
                } else if (op != spv::OpReturn && op != spv::OpNop) {
                    error = "unsupported instruction in SPIRV-Cross Typed shader subset";
                    return false;
                }
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
            if (!compiler.has_decoration(resource.id, spv::DecorationLocation))
                continue;
            const auto type = typed_type(compiler.get_type(resource.type_id));
            if (type != backend::TypedType::U32 && type != backend::TypedType::F32) continue;
            const uint32_t location = compiler.get_decoration(resource.id, spv::DecorationLocation);
            if (location >= 128) { error = "stage input location exceeds current Typed IR register subset"; return false; }
            const auto value = typed.input(type, static_cast<uint16_t>(location));
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
        std::vector<uint32_t> function_labels;
        bool in_function = false;
        uint32_t current_label = 0;
        for (size_t offset = 5; offset < words.size();) {
            const uint32_t first = words[offset];
            const uint16_t count = static_cast<uint16_t>(first >> 16);
            const uint16_t op = static_cast<uint16_t>(first);
            if (!count || offset + count > words.size()) { error = "malformed SPIR-V instruction stream"; return false; }
            const uint32_t *args = words.data() + offset + 1;
            if (op == spv::OpFunction && count >= 3) in_function = args[1] == function_id;
            else if (in_function && op == spv::OpLabel && count == 2) {
                current_label = args[0];
                function_labels.push_back(current_label);
            }
            else if (in_function && op == spv::OpKill && current_label) kill_labels.insert(current_label);
            else if (in_function && op == spv::OpFunctionEnd) { in_function = false; current_label = 0; }
            offset += count;
        }

        const bool discard_cfg = !kill_labels.empty();
        std::unordered_map<uint32_t, uint16_t> block_labels;
        if (!discard_cfg) {
            for (uint32_t id : function_labels) {
                const uint16_t label = typed.make_label();
                if (label == std::numeric_limits<uint16_t>::max()) {
                    error = "Typed IR control-flow label table overflow";
                    return false;
                }
                block_labels[id] = label;
            }
        }

        bool emitted_discard = false;
        bool emitted_branch = false;
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

            if (op == spv::OpLabel && count == 2) {
                if (!discard_cfg) {
                    const auto label = block_labels.find(args[0]);
                    if (label == block_labels.end() || !typed.bind_label(label->second)) {
                        error = "failed to bind Typed IR control-flow label";
                        return false;
                    }
                }
            } else if (op == spv::OpLoad && count >= 4) {
                backend::TypedValue source{};
                if ((!is_scalar_u32(compiler,args[0]) && !is_scalar_f32(compiler,args[0])) ||
                    !lookup(values, args[2], source)) {
                    error = "Typed IR adapter supports only direct scalar U32/F32 resource loads";
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
                } else if (map_float_compare(op, compare)) {
                    if (count != 5) { error = "invalid F32 compare instruction"; return false; }
                    backend::TypedValue lhs{}, rhs{};
                    if (!lookup(values,args[2],lhs) || !lookup(values,args[3],rhs) ||
                        lhs.type()!=backend::TypedType::F32 || rhs.type()!=backend::TypedType::F32) {
                        error = "unresolved F32 compare operand"; return false;
                    }
                    const auto dst=typed.make_predicate();
                    if (!typed.emit<backend::TypedOpcode::Compare>(static_cast<uint8_t>(compare),dst,lhs,rhs)) {
                        error = "failed to emit Typed IR F32 compare"; return false;
                    }
                    values[args[1]]=dst;
                } else if (op == spv::OpBranchConditional) {
                    if (count != 4) { error = "invalid conditional branch"; return false; }
                    backend::TypedValue predicate{};
                    if (!lookup(values, args[0], predicate) || predicate.kind() != backend::TypedValueKind::Predicate) {
                        error = "branch condition is not a Typed IR predicate"; return false;
                    }
                    if (discard_cfg) {
                        const bool true_kills = kill_labels.count(args[1]) != 0;
                        const bool false_kills = kill_labels.count(args[2]) != 0;
                        if (true_kills == false_kills) { error = "conditional branch is not a single discard edge"; return false; }
                        if (false_kills) predicate = backend::TypedValue::predicate(predicate.id(), !predicate.inverted());
                        if (!typed.emit<backend::TypedOpcode::Discard>(0, {}, predicate)) {
                            error = "failed to emit Typed IR discard"; return false;
                        }
                        emitted_discard = true;
                    } else {
                        const auto true_label = block_labels.find(args[1]);
                        const auto false_label = block_labels.find(args[2]);
                        if (true_label == block_labels.end() || false_label == block_labels.end() ||
                            !typed.branch(true_label->second, predicate) || !typed.jump(false_label->second)) {
                            error = "failed to emit Typed IR conditional control flow";
                            return false;
                        }
                        emitted_branch = true;
                    }
                } else if (op == spv::OpBranch) {
                    if (count != 2) { error = "invalid unconditional branch"; return false; }
                    if (!discard_cfg) {
                        const auto target = block_labels.find(args[0]);
                        if (target == block_labels.end() || !typed.jump(target->second)) {
                            error = "failed to emit Typed IR jump";
                            return false;
                        }
                        emitted_branch = true;
                    }
                } else if (op != spv::OpSelectionMerge &&
                           op != spv::OpKill && op != spv::OpReturn && op != spv::OpNop) {
                    error = "unsupported instruction in SPIRV-Cross Typed IR fragment subset";
                    return false;
                }
            }
            offset += count;
        }

        if (discard_cfg && !emitted_discard) { error = "Typed IR adapter did not emit discard"; return false; }
        if (!discard_cfg && !emitted_branch) { error = "Typed IR adapter found no supported fragment control flow"; return false; }
        return true;
    } catch (const std::exception &e) {
        error = std::string("SPIRV-Cross Typed IR adapter: ") + e.what();
        return false;
    }
#endif
}

} // namespace vsc
