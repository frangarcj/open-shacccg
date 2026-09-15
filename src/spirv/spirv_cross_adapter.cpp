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

bool is_f32_mat3(const spirv_cross::SPIRType &type) {
    return type.basetype == spirv_cross::SPIRType::Float && type.width == 32 &&
        type.vecsize == 3 && type.columns == 3 && type.array.empty();
}

uint32_t f32_mat4_array_count(const spirv_cross::SPIRType &type) {
    if (type.basetype != spirv_cross::SPIRType::Float || type.width != 32 ||
        type.vecsize != 4 || type.columns != 4 || type.array.size() != 1)
        return 0;
    const uint32_t count=type.array[0];
    return count>=1 && count<=3 ? count : 0;
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
    if (base.find("SPRITECOORD") != std::string::npos)
        return backend::TypedSemantic::PointCoord;
    if (base.find("PSIZE") != std::string::npos || base.find("POINTSIZE") != std::string::npos)
        return backend::TypedSemantic::PointSize;
    if (base.find("CLP") != std::string::npos || base.find("CLIP") != std::string::npos)
        return backend::TypedSemantic::ClipDistance;
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
            std::vector<uint16_t> matrix_resources;
            std::string name;
            bool matrix = false;
            bool matrix_array = false;
            bool vector_array_one = false;
            bool supported = false;
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
        struct SamplerArrayInfo {
            std::string name;
            uint16_t binding = 0;
            uint32_t count = 0;
            std::vector<backend::TypedValue> values;
        };
        struct RgbInsertChain {
            backend::TypedValue base{};
            backend::TypedValue rgb{};
            uint8_t mask = 0;
        };

        std::unordered_map<uint32_t, backend::TypedValue> values;
        std::unordered_map<uint32_t, std::vector<UniformMember>> uniform_blocks;
        std::unordered_map<uint32_t, UniformMember> access_chain_members;
        std::unordered_map<uint32_t, SamplerArrayInfo> sampler_arrays;
        std::unordered_map<uint32_t, backend::TypedValue> sampler_access_chains;
        std::unordered_map<uint32_t, ExtractInfo> input_access_chains;
        std::unordered_map<uint32_t, uint16_t> matrix_values;
        std::unordered_map<uint32_t, std::array<uint16_t,2>> matrix_products;
        std::unordered_map<uint32_t, ExtractInfo> extracts;
        std::unordered_map<uint32_t, RgbInsertChain> rgb_insert_chains;
        std::unordered_set<uint32_t> composite_insert_operands;
        std::unordered_set<uint32_t> composite_construct_operands;
        std::unordered_map<uint32_t, uint32_t> constants;
        std::unordered_map<uint32_t, std::array<uint32_t,4>> float_vector_constants;
        std::unordered_set<uint32_t> float_ones;
        std::unordered_set<uint32_t> glsl450_imports;
        std::unordered_map<uint32_t, uint16_t> outputs;
        std::unordered_map<uint32_t, PhiSink> phi_sinks;
        std::unordered_map<uint32_t, SelectSink> select_sinks;
        std::unordered_map<uint32_t, std::array<backend::TypedValue,2>> logical_ands;
        std::unordered_map<uint32_t, uint16_t> block_labels;
        std::unordered_map<uint32_t, uint32_t> loop_headers;
        std::unordered_set<uint32_t> kill_labels;
        std::unordered_set<uint32_t> s32_loop_state_ids;
        std::unordered_map<uint32_t, backend::TypedValue> function_array_one_states;
        std::unordered_map<uint32_t, backend::TypedValue> function_array_one_accesses;
        bool structured_control=false;
        std::unordered_set<uint32_t> narrow_u16_ids;
        std::unordered_set<uint32_t> narrow_s16_ids;

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
            std::unordered_set<uint32_t> active_members;
            for (const auto &range:compiler.get_active_buffer_ranges(resource.id))
                active_members.insert(range.index);
            auto &members = uniform_blocks[resource.id];
            members.resize(struct_type.member_types.size());
            uint16_t packed_word=0;
            auto allocate_uniform_words=[&](uint16_t words,bool register_aligned) -> uint16_t {
                if (register_aligned || static_cast<uint16_t>((packed_word&1u)+words)>2u)
                    packed_word=static_cast<uint16_t>((packed_word+1u)&~1u);
                const uint16_t index=packed_word;
                packed_word=static_cast<uint16_t>(packed_word+words);
                return index;
            };
            for (uint32_t member = 0; member < struct_type.member_types.size(); ++member) {
                // Sony drops dead uniforms from reflection/codegen. Keep the
                // aggregate slot so access-chain indices remain stable, but do
                // not materialize a Typed resource for an inactive member.
                if (!active_members.count(member)) continue;
                const auto &member_type = compiler.get_type(struct_type.member_types[member]);
                const uint32_t byte_offset = compiler.type_struct_member_offset(struct_type, member);
                if ((byte_offset & 3u) || byte_offset / 4u > std::numeric_limits<uint16_t>::max()) {
                    error = "uniform member offset is outside the Typed IR resource range";
                    return false;
                }
                std::string member_name = compiler.get_member_name(resource.base_type_id, member);
                if (member_name.empty()) member_name = resource.name + ".member" + std::to_string(member);
                uint16_t resource_id = 0;
                if (is_f32_mat3(member_type)) {
                    // Sony reflects float3x3 as three float3 rows/columns while
                    // reserving four F32 words per element (12 words total).
                    const uint16_t index=allocate_uniform_words(12,true);
                    if (!add_resource(backend::TypedResourceKind::Matrix3, {}, backend::TypedType::F32x3,
                                      member_name, index, backend::TypedSemantic::None, 0, resource_id)) return false;
                    members[member] = {resource_id, {}, {resource_id}, member_name, true, false, false, true};
                    continue;
                }
                if (is_f32_mat4(member_type)) {
                    const uint16_t index=allocate_uniform_words(16,true);
                    if (!add_resource(backend::TypedResourceKind::Matrix4, {}, backend::TypedType::F32x4,
                                      member_name, index, backend::TypedSemantic::None, 0, resource_id)) return false;
                    members[member] = {resource_id, {}, {resource_id}, member_name, true, false, false, true};
                    continue;
                }
                // vitaGL's fixed-function generator emits Ktexmat as mat4[1..3].
                // Sony lays consecutive elements 16 F32 words apart;
                // represent each element as a Matrix4 resource while retaining
                // the aggregate member for constant-index access chains.
                const uint32_t matrix_count=f32_mat4_array_count(member_type);
                if (matrix_count) {
                    const uint16_t index=allocate_uniform_words(static_cast<uint16_t>(16u*matrix_count),true);
                    UniformMember info{};
                    info.name=member_name;
                    info.matrix=true;
                    info.matrix_array=true;
                    info.supported=true;
                    for (uint32_t element=0;element<matrix_count;++element) {
                        const uint32_t word_index=static_cast<uint32_t>(index)+element*16u;
                        if (word_index>std::numeric_limits<uint16_t>::max()) {
                            error="mat4 array element offset exceeds Typed IR resource range";
                            return false;
                        }
                        uint16_t element_resource=0;
                        if (!add_resource(backend::TypedResourceKind::Matrix4, {}, backend::TypedType::F32x4,
                                          member_name, static_cast<uint16_t>(word_index),
                                          backend::TypedSemantic::None, 0, element_resource)) return false;
                        info.matrix_resources.push_back(element_resource);
                    }
                    info.resource=info.matrix_resources.front();
                    members[member]=std::move(info);
                    continue;
                }
                // Cg fixed-function lighting commonly uses float4[1]/float3[1]
                // for a single enabled light. Sony reflects these as one
                // parameter (array_size=1) but keeps array-style access chains.
                // Collapse only the independently observed one-element vector
                // case; dynamic indices are equivalent to element zero for all
                // defined accesses to an array of length one.
                if (member_type.basetype==spirv_cross::SPIRType::Float && member_type.width==32 &&
                    member_type.columns==1 && member_type.array.size()==1 && member_type.array[0]==1 &&
                    (member_type.vecsize==3 || member_type.vecsize==4)) {
                    const auto type=member_type.vecsize==3 ? backend::TypedType::F32x3 : backend::TypedType::F32x4;
                    const uint16_t words=4; // Sony gives one-element vector arrays a four-word slot.
                    const uint16_t index=allocate_uniform_words(words,true);
                    const auto value=program.uniform(type,index);
                    if (value.kind()==backend::TypedValueKind::None) {
                        error="failed to create one-element vector-array Typed uniform";
                        return false;
                    }
                    if (!add_resource(backend::TypedResourceKind::Uniform,value,type,member_name,index,
                                      backend::TypedSemantic::None,0,resource_id)) return false;
                    UniformMember info{};
                    info.resource=resource_id;
                    info.value=value;
                    info.name=member_name;
                    info.vector_array_one=true;
                    info.supported=true;
                    members[member]=std::move(info);
                    continue;
                }
                const auto type = typed_type(member_type);
                // Active but unsupported members fail closed if reached below.
                if (type == backend::TypedType::Invalid) {
                    members[member].name=member_name;
                    continue;
                }
                const uint8_t components=backend::typed_component_count(type);
                if (!components || components>4) {
                    error="active uniform member has unsupported packed width";
                    return false;
                }
                const uint16_t index=allocate_uniform_words(components,components==4);
                const auto value = program.uniform(type, index);
                if (value.kind() == backend::TypedValueKind::None) { error = "failed to create Typed IR uniform"; return false; }
                if (!add_resource(backend::TypedResourceKind::Uniform, value, type, member_name, index,
                                  backend::TypedSemantic::None, 0, resource_id)) return false;
                members[member] = {resource_id, value, {}, member_name, false, false, false, true};
            }
        }

        for (const auto &resource : reflected.sampled_images) {
            uint32_t binding = 0;
            if (compiler.has_decoration(resource.id, spv::DecorationBinding))
                binding = compiler.get_decoration(resource.id, spv::DecorationBinding);
            if (binding > std::numeric_limits<uint16_t>::max()) { error = "sampler binding is too large"; return false; }
            const auto &resource_type=compiler.get_type(resource.type_id);
            if (!resource_type.array.empty()) {
                if (resource_type.array.size()!=1 || resource_type.array[0]<1 || resource_type.array[0]>3) {
                    error="sampler array is outside the validated 1-3 element subset";
                    return false;
                }
                SamplerArrayInfo info{};
                info.name=resource.name;
                info.binding=static_cast<uint16_t>(binding);
                info.count=resource_type.array[0];
                info.values.resize(info.count);
                sampler_arrays.emplace(resource.id,std::move(info));
                continue;
            }
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
            const auto &spv_type=compiler.get_type(resource.type_id);
            const bool scalar_array_one=spv_type.basetype==spirv_cross::SPIRType::Float && spv_type.width==32 &&
                spv_type.vecsize==1 && spv_type.columns==1 && spv_type.array.size()==1 && spv_type.array[0]==1;
            const auto type = scalar_array_one ? backend::TypedType::F32 : typed_type(spv_type);
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
                !compiler.has_decoration(id, spv::DecorationBuiltIn) || outputs.count(id))
                continue;
            const auto builtin=compiler.get_decoration(id,spv::DecorationBuiltIn);
            const auto type = typed_type(compiler.get_type_from_variable(id));
            backend::TypedSemantic semantic=backend::TypedSemantic::None;
            const char *fallback=nullptr;
            if (builtin==spv::BuiltInPosition) {
                if (type!=backend::TypedType::F32x4) { error="BuiltIn Position is not float4"; return false; }
                semantic=backend::TypedSemantic::Position;
                fallback="position";
            } else if (builtin==spv::BuiltInPointSize) {
                if (type!=backend::TypedType::F32) { error="BuiltIn PointSize is not scalar float"; return false; }
                semantic=backend::TypedSemantic::PointSize;
                fallback="psize";
            } else {
                continue;
            }
            std::string output_name = compiler.get_name(id);
            if (output_name.empty()) output_name = fallback;
            uint16_t resource_id = 0;
            if (!add_resource(backend::TypedResourceKind::Output, {}, type, output_name, 0,
                              semantic, 0, resource_id)) return false;
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
            } else if (op==spv::OpUndef && count==3 &&
                       typed_type(compiler.get_type(args[0]))==backend::TypedType::F32x4) {
                // SPIRV-Tools uses an undef float4 as the base for partial RGB
                // construction. Choosing zero is a legal concretization of
                // undef and gives the Typed/Machine path a stable value for the
                // lanes that are subsequently overwritten.
                const std::array<uint32_t,4> zero={{0,0,0,0}};
                const auto value=program.literal_f32x4(zero);
                if (value.kind()==backend::TypedValueKind::None) {
                    error="failed to materialize F32x4 undef value";
                    return false;
                }
                values[args[1]]=value;
            } else if (op==spv::OpCompositeInsert && count==6) {
                composite_insert_operands.insert(args[2]);
            } else if (op==spv::OpCompositeConstruct && count>=4) {
                for (uint16_t operand=2;operand<count-1;++operand)
                    composite_construct_operands.insert(args[operand]);
            }
            offset += count;
        }

        // Pre-scan structured control flow for both stages. Fragment-only
        // output/select sinking remains guarded below; vertex shaders still
        // need labels, loop headers and loop-state phis so their BR structure
        // can reach the shared Typed/Machine CFG lowering.
        {
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
                } else if (scan_function && op==spv::OpKill && scan_label) {
                    kill_labels.insert(scan_label);
                    has_control=true;
                } else if (stage==backend::TypedStage::Fragment && scan_function && op==spv::OpSelect) {
                    if (count!=6) {
                        error="only ordinary three-operand OpSelect is supported";
                        return false;
                    }
                    select_sinks[args[1]]={args[2],args[3],args[4],std::numeric_limits<uint16_t>::max()};
                } else if (stage==backend::TypedStage::Fragment && scan_function && op==spv::OpStore && count>=3) {
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
                structured_control=true;
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

            if (op==spv::OpVariable && count>=4 && args[2]==spv::StorageClassFunction) {
                const auto &variable_type=compiler.get_type_from_variable(args[1]);
                if (variable_type.basetype==spirv_cross::SPIRType::Float && variable_type.width==32 &&
                    variable_type.vecsize==1 && variable_type.columns==1 && variable_type.array.size()==1 &&
                    variable_type.array[0]==1) {
                    const auto zero=program.literal_f32(0);
                    const auto state=program.make_value<backend::TypedType::F32>();
                    if (zero.kind()==backend::TypedValueKind::None || state.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::StateInit>(0,state,zero)) {
                        error="failed to create scalar state for one-element function array";
                        return false;
                    }
                    function_array_one_states[args[1]]=state;
                }
            } else if (op == spv::OpLabel && count == 2) {
                current_label=args[0];
                if (!block_labels.empty()) {
                    const auto label=block_labels.find(current_label);
                    if (label==block_labels.end() || !program.bind_label(label->second)) {
                        error="failed to bind Typed shader control-flow label";
                        return false;
                    }
                }
            } else if (op == spv::OpAccessChain && count >= 4) {
                if (const auto state=function_array_one_states.find(args[2]);state!=function_array_one_states.end()) {
                    function_array_one_accesses[args[1]]=state->second;
                    offset+=count;
                    continue;
                }
                const auto block = uniform_blocks.find(args[2]);
                const auto index_it = constants.find(args[3]);
                const auto sampler_array=sampler_arrays.find(args[2]);
                if (sampler_array!=sampler_arrays.end() && index_it!=constants.end()) {
                    const uint32_t element=index_it->second;
                    auto &info=sampler_array->second;
                    if (count!=5 || element>=info.count ||
                        static_cast<uint32_t>(info.binding)+element>std::numeric_limits<uint16_t>::max()) {
                        error="sampler-array access is outside constant index 0..2 subset";
                        return false;
                    }
                    auto &value=info.values[element];
                    if (value.kind()==backend::TypedValueKind::None) {
                        const uint16_t element_binding=static_cast<uint16_t>(info.binding+element);
                        value=program.sampler(element_binding);
                        if (value.kind()==backend::TypedValueKind::None) {
                            error="failed to create Typed sampler-array element";
                            return false;
                        }
                        uint16_t resource_id=0;
                        if (!add_resource(backend::TypedResourceKind::Sampler2D,value,backend::TypedType::Sampler2D,
                                          info.name,element_binding,backend::TypedSemantic::None,0,resource_id))
                            return false;
                    }
                    sampler_access_chains[args[1]]=value;
                } else if (block != uniform_blocks.end() && index_it != constants.end()) {
                    const uint32_t member = index_it->second;
                    if (member >= block->second.size()) { error = "uniform access-chain member is out of range"; return false; }
                    const auto &uniform=block->second[member];
                    if (!uniform.supported) {
                        error="used uniform member type is unsupported by Typed IR";
                        if (!uniform.name.empty()) error += ": " + uniform.name;
                        return false;
                    }
                    if (count==5) {
                        access_chain_members[args[1]]=uniform;
                    } else if (count==6 && uniform.matrix && uniform.matrix_array) {
                        const auto element_it=constants.find(args[4]);
                        if (element_it==constants.end() || element_it->second>=uniform.matrix_resources.size()) {
                            error="mat4 array access is unresolved or outside the validated 1-2 element subset";
                            return false;
                        }
                        auto element=uniform;
                        element.resource=uniform.matrix_resources[element_it->second];
                        element.matrix_array=false;
                        access_chain_members[args[1]]=std::move(element);
                    } else if (count==6 && uniform.vector_array_one) {
                        // Array length is exactly one. Keep a vector access even
                        // when SPIRV-Tools preserves a dynamic loop index.
                        access_chain_members[args[1]]=uniform;
                    } else if (count==7 && uniform.vector_array_one) {
                        const auto component_it=constants.find(args[5]);
                        const uint8_t components=backend::typed_component_count(uniform.value.type());
                        if (component_it==constants.end() || component_it->second>=components) {
                            error="one-element vector-array component is unresolved or out of range";
                            return false;
                        }
                        input_access_chains[args[1]]={uniform.value,component_it->second};
                    } else if (count==6 && !uniform.matrix) {
                        const auto component_it=constants.find(args[4]);
                        const uint8_t components=backend::typed_component_count(uniform.value.type());
                        if (component_it==constants.end() || component_it->second>=components) {
                            error="uniform access-chain component is unresolved or out of range";
                            return false;
                        }
                        input_access_chains[args[1]]={uniform.value,component_it->second};
                    } else {
                        error="uniform access-chain shape is outside the validated scalar/vector subset";
                        return false;
                    }
                } else if (const auto aggregate=access_chain_members.find(args[2]);
                           aggregate!=access_chain_members.end() && aggregate->second.vector_array_one) {
                    // A one-element vector array may retain a dynamic loop
                    // index in optimized SPIR-V. Any defined access is element
                    // zero, so collapse the element pointer to the vector.
                    access_chain_members[args[1]]=aggregate->second;
                } else if (index_it != constants.end()) {
                    const auto matrix_array=access_chain_members.find(args[2]);
                    if (matrix_array!=access_chain_members.end() && matrix_array->second.matrix &&
                        matrix_array->second.matrix_array) {
                        if (index_it->second>=matrix_array->second.matrix_resources.size()) {
                            error="mat4 array access is unresolved or outside the validated 1-2 element subset";
                            return false;
                        }
                        auto element=matrix_array->second;
                        element.resource=element.matrix_resources[index_it->second];
                        element.matrix_array=false;
                        access_chain_members[args[1]]=std::move(element);
                    } else if (const auto input=values.find(args[2]); input!=values.end() && typed_is_float(input->second.type())) {
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
                } else if (auto it=function_array_one_states.find(args[2]);it!=function_array_one_states.end()) {
                    values[args[1]]=it->second;
                } else if (auto it=function_array_one_accesses.find(args[2]);it!=function_array_one_accesses.end()) {
                    values[args[1]]=it->second;
                } else if (auto it=sampler_access_chains.find(args[2]); it!=sampler_access_chains.end()) {
                    values[args[1]]=it->second;
                } else if (auto it = access_chain_members.find(args[2]); it != access_chain_members.end()) {
                    if (it->second.matrix) matrix_values[args[1]] = it->second.resource;
                    else values[args[1]] = it->second.value;
                } else if (auto it=input_access_chains.find(args[2]); it!=input_access_chains.end()) {
                    if (typed_type(compiler.get_type(args[0]))!=backend::TypedType::F32 ||
                        it->second.component>=backend::typed_component_count(it->second.source.type())) {
                        error="float-vector access-chain component is outside the validated scalar subset";
                        return false;
                    }
                    extracts[args[1]]=it->second;
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (!program.emit<backend::TypedOpcode::FloatExtract>(static_cast<uint8_t>(it->second.component),dst,it->second.source)) {
                        error="failed to emit Typed IR float component extraction";
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
                    // SPIRV-Tools rewrites `float4.rgb = float3` as three
                    // extract/insert pairs. Keep those extracts symbolic so the
                    // complete chain can become one Typed FloatReplaceRGB.
                    if (source->second.type()==backend::TypedType::F32x3 &&
                        composite_insert_operands.count(args[1])) {
                        offset+=count;
                        continue;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatExtract>(static_cast<uint8_t>(args[3]),dst,source->second)) {
                        error="failed to emit Typed float component extraction";
                        return false;
                    }
                    values[args[1]]=dst;
                }
            } else if (op==spv::OpCompositeInsert && count==6) {
                const auto result_type=typed_type(compiler.get_type(args[0]));
                const auto extracted=extracts.find(args[2]);
                if (result_type!=backend::TypedType::F32x4 || extracted==extracts.end() ||
                    extracted->second.source.type()!=backend::TypedType::F32x3 || args[4]>=3 ||
                    extracted->second.component!=args[4]) {
                    error="OpCompositeInsert is outside the validated float3-to-RGB chain";
                    return false;
                }
                RgbInsertChain chain{};
                if (args[4]==0) {
                    const auto base=values.find(args[3]);
                    if (base==values.end() || base->second.type()!=backend::TypedType::F32x4) {
                        error="RGB insert chain has unresolved float4 base";
                        return false;
                    }
                    chain={base->second,extracted->second.source,1};
                } else {
                    const auto prior=rgb_insert_chains.find(args[3]);
                    const uint8_t expected_mask=static_cast<uint8_t>((1u<<args[4])-1u);
                    if (prior==rgb_insert_chains.end() || prior->second.mask!=expected_mask ||
                        prior->second.rgb.bits!=extracted->second.source.bits) {
                        error="RGB insert chain is non-contiguous or mixes float3 sources";
                        return false;
                    }
                    chain=prior->second;
                    chain.mask=static_cast<uint8_t>(chain.mask | (1u<<args[4]));
                }
                rgb_insert_chains[args[1]]=chain;
                if (chain.mask==0x7) {
                    const auto dst=program.make_value<backend::TypedType::F32x4>();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatReplaceRGB>(0,dst,chain.base,chain.rgb)) {
                        error="failed to emit Typed RGB replacement";
                        return false;
                    }
                    values[args[1]]=dst;
                }
            } else if (op == spv::OpVectorShuffle) {
                const auto result_type = typed_type(compiler.get_type(args[0]));
                const auto source = values.find(args[2]);
                if (source == values.end() || source->second.type()!=backend::TypedType::F32x4 ||
                    args[2]!=args[3]) {
                    error = "vector shuffle is outside the validated single-source F32 subset";
                    return false;
                }
                if (count==7 && result_type==backend::TypedType::F32x2 && args[4]==0 && args[5]==1) {
                    const auto dst=program.make_value<backend::TypedType::F32x2>();
                    if (!program.emit<backend::TypedOpcode::FloatSwizzle>(
                            static_cast<uint8_t>(backend::TypedFloatSwizzleOp::XY),dst,source->second)) {
                        error="failed to emit Typed IR float4.xy swizzle";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (count==8 && result_type==backend::TypedType::F32x3 &&
                           args[4]==0 && args[5]==1 && args[6]==2) {
                    const auto dst=program.make_value<backend::TypedType::F32x3>();
                    if (!program.emit<backend::TypedOpcode::FloatSwizzle>(
                            static_cast<uint8_t>(backend::TypedFloatSwizzleOp::XYZ),dst,source->second)) {
                        error="failed to emit Typed IR float4.xyz swizzle";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (count==9 && result_type==backend::TypedType::F32x4) {
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
                } else {
                    error = "vector shuffle result shape is outside the validated F32 subset";
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
                if (result_type==backend::TypedType::F32x3 && count==6) {
                    std::array<backend::TypedValue,3> components{};
                    bool compose=true;
                    for (uint8_t lane=0;compose && lane<3;++lane) {
                        const uint32_t operand=args[2+lane];
                        if (auto value=values.find(operand); value!=values.end() &&
                            value->second.type()==backend::TypedType::F32) {
                            components[lane]=value->second;
                        } else if (auto constant=constants.find(operand); constant!=constants.end()) {
                            components[lane]=program.literal_f32(constant->second);
                        } else {
                            compose=false;
                        }
                    }
                    if (!compose) {
                        error="float3 composite has unresolved scalar components";
                        return false;
                    }
                    const auto dst=program.compose_f32x3(components);
                    if (dst.kind()==backend::TypedValueKind::None) {
                        error="failed to emit Typed F32x3 composite";
                        return false;
                    }
                    values[args[1]]=dst;
                    offset+=count;
                    continue;
                }
                if (result_type != backend::TypedType::F32x4) { error = "unsupported composite construct result type"; return false; }
                if (count==7) {
                    const auto x=extracts.find(args[2]);
                    const auto y=extracts.find(args[3]);
                    const auto z=extracts.find(args[4]);
                    const auto a=extracts.find(args[5]);
                    if (x!=extracts.end() && y!=extracts.end() && z!=extracts.end() && a!=extracts.end() &&
                        x->second.component==0 && y->second.component==1 && z->second.component==2 &&
                        x->second.source.bits==y->second.source.bits && x->second.source.bits==z->second.source.bits &&
                        x->second.source.type()==backend::TypedType::F32x3 &&
                        a->second.component==3 && a->second.source.type()==backend::TypedType::F32x4) {
                        const auto dst=program.make_value<backend::TypedType::F32x4>();
                        if (dst.kind()==backend::TypedValueKind::None ||
                            !program.emit<backend::TypedOpcode::FloatReplaceRGB>(0,dst,a->second.source,x->second.source)) {
                            error="failed to emit final float3 RGB + float alpha composition";
                            return false;
                        }
                        values[args[1]]=dst;
                        offset+=count;
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
                    std::array<backend::TypedValue,4> components{};
                    bool compose=count==7;
                    for (uint8_t lane=0;compose && lane<4;++lane) {
                        const uint32_t operand=args[2+lane];
                        if (auto value=values.find(operand); value!=values.end() &&
                            value->second.type()==backend::TypedType::F32) {
                            components[lane]=value->second;
                        } else if (auto constant=constants.find(operand); constant!=constants.end()) {
                            components[lane]=program.literal_f32(constant->second);
                        } else {
                            compose=false;
                        }
                        compose = compose && components[lane].kind()!=backend::TypedValueKind::None;
                    }
                    if (!compose) {
                        error = "composite construct is outside homogeneous-position/general F32x4 subset";
                        return false;
                    }
                    const auto dst=program.compose_f32x4(components);
                    if (dst.kind()==backend::TypedValueKind::None) {
                        error="failed to emit Typed F32x4 composite";
                        return false;
                    }
                    values[args[1]]=dst;
                    offset+=count;
                    continue;
                }
                const auto dst = program.make_value<backend::TypedType::F32x4>();
                if (!program.emit<backend::TypedOpcode::ConstructPosition>(0, dst, source)) {
                    error = "failed to emit Typed IR position construct"; return false;
                }
                values[args[1]] = dst;
            } else if (op==spv::OpMatrixTimesMatrix && count==5) {
                const auto lhs=matrix_values.find(args[2]);
                const auto rhs=matrix_values.find(args[3]);
                if (lhs==matrix_values.end() || rhs==matrix_values.end() ||
                    typed_type(compiler.get_type(args[0]))!=backend::TypedType::Invalid) {
                    // Matrix types deliberately do not have a compact TypedType;
                    // both operands must therefore be direct reflected matrices.
                    error="matrix-times-matrix operands are unresolved";
                    return false;
                }
                if (lhs->second>=typed.resources().size() || rhs->second>=typed.resources().size() ||
                    typed.resources()[lhs->second].kind!=backend::TypedResourceKind::Matrix4 ||
                    typed.resources()[rhs->second].kind!=backend::TypedResourceKind::Matrix4) {
                    error="matrix-times-matrix currently supports two float4x4 uniforms";
                    return false;
                }
                matrix_products[args[1]]={{lhs->second,rhs->second}};
            } else if ((op == spv::OpMatrixTimesVector || op == spv::OpVectorTimesMatrix) && count == 5) {
                const uint32_t matrix_id = op == spv::OpMatrixTimesVector ? args[2] : args[3];
                const uint32_t vector_id = op == spv::OpMatrixTimesVector ? args[3] : args[2];
                const auto matrix = matrix_values.find(matrix_id);
                const auto product = matrix_products.find(matrix_id);
                const auto vector = values.find(vector_id);
                if ((matrix == matrix_values.end() && product==matrix_products.end()) || vector == values.end()) {
                    error = "matrix/vector multiply operands are unresolved"; return false;
                }
                const auto dst_type = typed_type(compiler.get_type(args[0]));
                if (product!=matrix_products.end()) {
                    if (dst_type!=backend::TypedType::F32x4 || vector->second.type()!=backend::TypedType::F32x4) {
                        error="mat4 product-times-vector requires float4 input/result";
                        return false;
                    }
                    const uint16_t first=op==spv::OpVectorTimesMatrix ? product->second[0] : product->second[1];
                    const uint16_t second=op==spv::OpVectorTimesMatrix ? product->second[1] : product->second[0];
                    const auto tmp=program.make_value<backend::TypedType::F32x4>();
                    const auto dst=program.make_value<backend::TypedType::F32x4>();
                    if (tmp.kind()==backend::TypedValueKind::None || dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::TransformPosition>(0,tmp,vector->second,{},first) ||
                        !program.emit<backend::TypedOpcode::TransformPosition>(0,dst,tmp,{},second)) {
                        error="failed to decompose float4x4 product transform";
                        return false;
                    }
                    values[args[1]]=dst;
                    offset+=count;
                    continue;
                }
                if (matrix->second>=typed.resources().size()) {
                    error="matrix resource is out of range";
                    return false;
                }
                const auto &matrix_resource=typed.resources()[matrix->second];
                if (matrix_resource.kind==backend::TypedResourceKind::Matrix3) {
                    if (dst_type!=backend::TypedType::F32x3 || vector->second.type()!=backend::TypedType::F32x3) {
                        error="mat3-times-vector requires float3 input/result";
                        return false;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32x3>();
                    if (!program.emit<backend::TypedOpcode::TransformVector3>(0,dst,vector->second,{},matrix->second)) {
                        error="failed to emit Typed IR float3 matrix transform";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (matrix_resource.kind==backend::TypedResourceKind::Matrix4) {
                    if (dst_type != backend::TypedType::F32x4 || vector->second.type() != backend::TypedType::F32x4) {
                        error = "matrix-times-vector is outside the float4 subset"; return false;
                    }
                    const auto dst = program.make_value<backend::TypedType::F32x4>();
                    if (!program.emit<backend::TypedOpcode::TransformPosition>(0, dst, vector->second, {}, matrix->second)) {
                        error = "failed to emit Typed IR position transform"; return false;
                    }
                    values[args[1]] = dst;
                } else {
                    error="matrix multiply references unsupported matrix resource kind";
                    return false;
                }
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
                } else if (ext == GLSLstd450Log2) {
                    if (count!=6 || result_type!=backend::TypedType::F32) {
                        error="GLSL.std.450 Log2 is outside the validated scalar F32 subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    if (source==values.end() || source->second.type()!=backend::TypedType::F32) {
                        error="unresolved GLSL.std.450 Log2 operand";
                        return false;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(
                            static_cast<uint8_t>(backend::TypedFloatUnaryOp::Log2),dst,source->second)) {
                        error="failed to emit Typed IR Log2";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (ext == GLSLstd450Exp) {
                    if (count!=6 || result_type!=backend::TypedType::F32) {
                        error="GLSL.std.450 Exp is outside the validated scalar F32 subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    if (source==values.end() || source->second.type()!=backend::TypedType::F32) {
                        error="unresolved GLSL.std.450 Exp operand";
                        return false;
                    }
                    // Hardware VCOMP op2=3 is Exp2. Sony's Cg exp() profile
                    // scales by LOG2E first, so preserve that decomposition.
                    const auto log2e=program.literal_f32(0x3fb8aa3au);
                    const auto scaled=program.make_value<backend::TypedType::F32>();
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (log2e.kind()==backend::TypedValueKind::None || scaled.kind()==backend::TypedValueKind::None ||
                        dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Mul),scaled,source->second,log2e) ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(
                            static_cast<uint8_t>(backend::TypedFloatUnaryOp::Exp2),dst,scaled)) {
                        error="failed to lower scalar Exp through LOG2E*Exp2";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (ext == GLSLstd450Pow) {
                    if (count!=7) {
                        error="GLSL.std.450 Pow has an unexpected operand shape";
                        return false;
                    }
                    if (result_type==backend::TypedType::F32) {
                        const auto base=values.find(args[4]);
                        const auto exponent=values.find(args[5]);
                        if (base==values.end() || exponent==values.end() ||
                            base->second.type()!=backend::TypedType::F32 ||
                            exponent->second.type()!=backend::TypedType::F32) {
                            error="scalar Pow requires resolved F32 base/exponent operands";
                            return false;
                        }
                        const auto logarithm=program.make_value<backend::TypedType::F32>();
                        const auto scaled=program.make_value<backend::TypedType::F32>();
                        const auto dst=program.make_value<backend::TypedType::F32>();
                        if (logarithm.kind()==backend::TypedValueKind::None ||
                            scaled.kind()==backend::TypedValueKind::None || dst.kind()==backend::TypedValueKind::None ||
                            !program.emit<backend::TypedOpcode::FloatUnary>(
                                static_cast<uint8_t>(backend::TypedFloatUnaryOp::Log2),logarithm,base->second) ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Mul),scaled,logarithm,exponent->second) ||
                            !program.emit<backend::TypedOpcode::FloatUnary>(
                                static_cast<uint8_t>(backend::TypedFloatUnaryOp::Exp2),dst,scaled)) {
                            error="failed to lower scalar Pow through Log2/Mul/Exp2";
                            return false;
                        }
                        values[args[1]]=dst;
                        offset+=count;
                        continue;
                    }
                    if (result_type!=backend::TypedType::F32x3) {
                        error="GLSL.std.450 Pow is outside the validated scalar/float3 subset";
                        return false;
                    }
                    const auto base=values.find(args[4]);
                    const auto exponent=float_vector_constants.find(args[5]);
                    if (base==values.end() || base->second.type()!=backend::TypedType::F32x3 ||
                        exponent==float_vector_constants.end() ||
                        exponent->second[0]!=exponent->second[1] || exponent->second[0]!=exponent->second[2]) {
                        error="float3 Pow requires a resolved base and constant splat exponent";
                        return false;
                    }
                    const auto exponent_scalar=program.literal_f32(exponent->second[0]);
                    std::array<backend::TypedValue,3> components{};
                    bool lowered=exponent_scalar.kind()!=backend::TypedValueKind::None;
                    for (uint8_t lane=0;lowered && lane<3;++lane) {
                        const auto scalar=program.make_value<backend::TypedType::F32>();
                        const auto logarithm=program.make_value<backend::TypedType::F32>();
                        const auto scaled=program.make_value<backend::TypedType::F32>();
                        const auto powered=program.make_value<backend::TypedType::F32>();
                        lowered=scalar.kind()!=backend::TypedValueKind::None &&
                            logarithm.kind()!=backend::TypedValueKind::None && scaled.kind()!=backend::TypedValueKind::None &&
                            powered.kind()!=backend::TypedValueKind::None &&
                            program.emit<backend::TypedOpcode::FloatExtract>(lane,scalar,base->second) &&
                            program.emit<backend::TypedOpcode::FloatUnary>(
                                static_cast<uint8_t>(backend::TypedFloatUnaryOp::Log2),logarithm,scalar) &&
                            program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Mul),scaled,logarithm,exponent_scalar) &&
                            program.emit<backend::TypedOpcode::FloatUnary>(
                                static_cast<uint8_t>(backend::TypedFloatUnaryOp::Exp2),powered,scaled);
                        components[lane]=powered;
                    }
                    const auto dst=lowered ? program.compose_f32x3(components) : backend::TypedValue{};
                    if (!lowered || dst.kind()==backend::TypedValueKind::None) {
                        error="failed to lower float3 Pow through Log2/Mul/Exp2";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (ext==GLSLstd450Length) {
                    if (count!=6 || result_type!=backend::TypedType::F32) {
                        error="GLSL.std.450 Length is outside the validated scalar-result subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    if (source==values.end() || source->second.type()!=backend::TypedType::F32x3) {
                        error="GLSL.std.450 Length currently requires a resolved F32x3 operand";
                        return false;
                    }
                    std::array<backend::TypedValue,3> lane{};
                    std::array<backend::TypedValue,3> square{};
                    bool ok=true;
                    for (uint8_t i=0;i<3;++i) {
                        lane[i]=program.make_value<backend::TypedType::F32>();
                        square[i]=program.make_value<backend::TypedType::F32>();
                        ok = ok && lane[i].kind()!=backend::TypedValueKind::None &&
                            square[i].kind()!=backend::TypedValueKind::None &&
                            program.emit<backend::TypedOpcode::FloatExtract>(i,lane[i],source->second) &&
                            program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Mul),square[i],lane[i],lane[i]);
                    }
                    const auto sum_xy=program.make_value<backend::TypedType::F32>();
                    const auto sum=program.make_value<backend::TypedType::F32>();
                    const auto inverse_length=program.make_value<backend::TypedType::F32>();
                    const auto length=program.make_value<backend::TypedType::F32>();
                    if (!ok || sum_xy.kind()==backend::TypedValueKind::None || sum.kind()==backend::TypedValueKind::None ||
                        inverse_length.kind()==backend::TypedValueKind::None || length.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Add),
                            sum_xy,square[0],square[1]) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Add),
                            sum,sum_xy,square[2]) ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(static_cast<uint8_t>(backend::TypedFloatUnaryOp::Rsqrt),
                            inverse_length,sum) ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(static_cast<uint8_t>(backend::TypedFloatUnaryOp::Reciprocal),
                            length,inverse_length)) {
                        error="failed to lower F32x3 Length through sum/rsqrt/reciprocal";
                        return false;
                    }
                    values[args[1]]=length;
                } else if (ext==GLSLstd450Normalize) {
                    if (count!=6 || result_type!=backend::TypedType::F32x3) {
                        error="GLSL.std.450 Normalize is outside the validated F32x3 subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    if (source==values.end() || source->second.type()!=backend::TypedType::F32x3) {
                        error="unresolved GLSL.std.450 Normalize operand";
                        return false;
                    }
                    std::array<backend::TypedValue,3> lane{};
                    std::array<backend::TypedValue,3> square{};
                    bool ok=true;
                    for (uint8_t i=0;i<3;++i) {
                        lane[i]=program.make_value<backend::TypedType::F32>();
                        square[i]=program.make_value<backend::TypedType::F32>();
                        ok = ok && lane[i].kind()!=backend::TypedValueKind::None &&
                            square[i].kind()!=backend::TypedValueKind::None &&
                            program.emit<backend::TypedOpcode::FloatExtract>(i,lane[i],source->second) &&
                            program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Mul),square[i],lane[i],lane[i]);
                    }
                    const auto sum_xy=program.make_value<backend::TypedType::F32>();
                    const auto sum=program.make_value<backend::TypedType::F32>();
                    const auto inv=program.make_value<backend::TypedType::F32>();
                    const auto inv3=program.make_value<backend::TypedType::F32x3>();
                    const auto dst=program.make_value<backend::TypedType::F32x3>();
                    if (!ok || sum_xy.kind()==backend::TypedValueKind::None || sum.kind()==backend::TypedValueKind::None ||
                        inv.kind()==backend::TypedValueKind::None || inv3.kind()==backend::TypedValueKind::None ||
                        dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Add),
                            sum_xy,square[0],square[1]) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Add),
                            sum,sum_xy,square[2]) ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(static_cast<uint8_t>(backend::TypedFloatUnaryOp::Rsqrt),
                            inv,sum) ||
                        !program.emit<backend::TypedOpcode::FloatSplat>(0,inv3,inv) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Mul),
                            dst,source->second,inv3)) {
                        error="failed to lower F32x3 Normalize through dot/rsqrt/mul";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (ext == GLSLstd450Floor) {
                    if (count!=6 || result_type!=backend::TypedType::F32) {
                        error="GLSL.std.450 Floor is outside the validated scalar F32 subset";
                        return false;
                    }
                    const auto source=values.find(args[4]);
                    if (source==values.end() || source->second.type()!=backend::TypedType::F32) {
                        error="unresolved GLSL.std.450 Floor operand";
                        return false;
                    }
                    const auto dst=program.make_value<backend::TypedType::F32>();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatUnary>(
                            static_cast<uint8_t>(backend::TypedFloatUnaryOp::Floor),dst,source->second)) {
                        error="failed to emit Typed IR Floor";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (ext == GLSLstd450FMin || ext == GLSLstd450FMax) {
                    if (count != 7 || !result_float) {
                        error = "GLSL.std.450 min/max is outside the validated F32 subset";
                        return false;
                    }
                    auto resolve_float=[&](uint32_t id, backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id);it!=values.end() && it->second.type()==result_type) {
                            value=it->second;
                            return true;
                        }
                        if (result_type==backend::TypedType::F32) {
                            const auto constant=constants.find(id);
                            if (constant==constants.end()) return false;
                            value=program.literal_f32(constant->second);
                            return value.kind()!=backend::TypedValueKind::None;
                        }
                        return false;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (!resolve_float(args[4],lhs) || !resolve_float(args[5],rhs)) {
                        error = "unresolved GLSL.std.450 min/max operand";
                        return false;
                    }
                    const auto dst = program.make_value(result_type);
                    const auto float_op = ext == GLSLstd450FMin ?
                        backend::TypedFloatOp::Min : backend::TypedFloatOp::Max;
                    if (!program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(float_op), dst, lhs, rhs)) {
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
                    if (source==values.end() || source->second.type()!=result_type || !constant_bits(args[5],0u)) {
                        error="FClamp source/lower bound is outside the validated F32 subset";
                        return false;
                    }
                    if (constant_bits(args[6],0x3f800000u)) {
                        const auto dst=program.make_value(result_type);
                        if (!program.emit<backend::TypedOpcode::FloatUnary>(
                                static_cast<uint8_t>(backend::TypedFloatUnaryOp::Saturate),dst,source->second)) {
                            error="failed to emit Typed IR saturate";
                            return false;
                        }
                        values[args[1]]=dst;
                    } else if (result_scalar) {
                        const auto upper=values.find(args[6]);
                        if (upper==values.end() || upper->second.type()!=backend::TypedType::F32) {
                            error="scalar FClamp upper bound is unresolved or non-F32";
                            return false;
                        }
                        const auto zero=program.literal_f32(0);
                        const auto low=program.make_value<backend::TypedType::F32>();
                        const auto dst=program.make_value<backend::TypedType::F32>();
                        if (zero.kind()==backend::TypedValueKind::None || low.kind()==backend::TypedValueKind::None ||
                            dst.kind()==backend::TypedValueKind::None ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Max),low,source->second,zero) ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(
                                static_cast<uint8_t>(backend::TypedFloatOp::Min),dst,low,upper->second)) {
                            error="failed to lower scalar FClamp to Typed min/max";
                            return false;
                        }
                        values[args[1]]=dst;
                    } else {
                        error="FClamp bounds are outside the validated vector zero/one subset";
                        return false;
                    }
                } else if (ext==GLSLstd450FMix) {
                    if (count!=8 || !result_float || result_scalar) {
                        error="GLSL.std.450 FMix is outside the validated F32-vector subset";
                        return false;
                    }
                    const auto x=values.find(args[4]);
                    const auto y=values.find(args[5]);
                    const auto a=values.find(args[6]);
                    if (x==values.end() || y==values.end() || a==values.end() ||
                        x->second.type()!=result_type || y->second.type()!=result_type || a->second.type()!=result_type) {
                        error="FMix operands are unresolved or type-mismatched";
                        return false;
                    }
                    const auto delta=program.make_value(result_type);
                    const auto scaled=program.make_value(result_type);
                    const auto dst=program.make_value(result_type);
                    if (delta.kind()==backend::TypedValueKind::None || scaled.kind()==backend::TypedValueKind::None ||
                        dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Sub),delta,y->second,x->second) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Mul),scaled,delta,a->second) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Add),dst,x->second,scaled)) {
                        error="failed to desugar F32-vector FMix";
                        return false;
                    }
                    values[args[1]]=dst;
                } else {
                    error = "GLSL.std.450 instruction is not in the validated Typed IR subset: " +
                        std::to_string(ext);
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
            } else if (op==spv::OpBitcast) {
                if (count!=4) { error="invalid scalar bitcast instruction"; return false; }
                const auto result_type=typed_type(compiler.get_type(args[0]));
                const auto source=values.find(args[2]);
                const auto scalar32=[](backend::TypedType type) {
                    return type==backend::TypedType::F32 || type==backend::TypedType::U32 || type==backend::TypedType::S32;
                };
                if (!scalar32(result_type) || source==values.end() || !scalar32(source->second.type()) ||
                    result_type==source->second.type()) {
                    error="OpBitcast is outside the scalar F32/U32/S32 reinterpretation subset";
                    return false;
                }
                const auto dst=program.make_value(result_type);
                if (dst.kind()==backend::TypedValueKind::None ||
                    !program.emit<backend::TypedOpcode::Bitcast>(0,dst,source->second)) {
                    error="failed to emit Typed scalar 32-bit bitcast";
                    return false;
                }
                values[args[1]]=dst;
            } else if (op == spv::OpConvertUToF) {
                if (count!=4 || typed_type(compiler.get_type(args[0]))!=backend::TypedType::F32 ||
                    !narrow_u16_ids.count(args[2])) {
                    error="unsigned-to-float conversion is outside the validated narrow U16 subset";
                    return false;
                }
                const auto source=values.find(args[2]);
                if (source==values.end() || source->second.type()!=backend::TypedType::U32) {
                    error="narrow U16 conversion source is unresolved";
                    return false;
                }
                const auto dst=program.make_value<backend::TypedType::F32>();
                if (dst.kind()==backend::TypedValueKind::None ||
                    !program.emit<backend::TypedOpcode::Narrow16ToFloat>(0,dst,source->second)) {
                    error="failed to emit narrow U16->F32 conversion";
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
                const bool narrow=narrow_s16_ids.count(args[2])!=0;
                if (dst.kind()==backend::TypedValueKind::None ||
                    !(narrow ? program.emit<backend::TypedOpcode::Narrow16ToFloat>(1,dst,source->second) :
                              program.emit<backend::TypedOpcode::S32ToFloat>(0,dst,source->second))) {
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
                    auto constant_is=[&](uint32_t id,uint32_t expected) {
                        const auto it=constants.find(id);
                        return it!=constants.end() && it->second==expected;
                    };
                    if (result_type==backend::TypedType::U32 && bitwise==usse::BitwiseOp::And &&
                        (constant_is(args[2],0xffffu) || constant_is(args[3],0xffffu)))
                        narrow_u16_ids.insert(args[1]);
                    if (result_type==backend::TypedType::S32 && bitwise==usse::BitwiseOp::ArithmeticShiftRight &&
                        constant_is(args[3],16u))
                        narrow_s16_ids.insert(args[1]);
                } else if (map_compare(op,compare)) {
                    if (count!=5) { error="invalid scalar integer compare instruction"; return false; }
                    auto resolve_u32=[&](uint32_t id, backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id); it!=values.end() && it->second.type()==backend::TypedType::U32) {
                            value=it->second;
                            return true;
                        }
                        const auto constant=constants.find(id);
                        if (constant==constants.end()) return false;
                        value=program.literal_u32(constant->second);
                        return value.kind()!=backend::TypedValueKind::None;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (!resolve_u32(args[2],lhs) || !resolve_u32(args[3],rhs)) {
                        error="integer compare operands are outside the validated scalar U32 subset";
                        return false;
                    }
                    const auto dst=program.make_predicate();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::Compare>(static_cast<uint8_t>(compare),dst,lhs,rhs)) {
                        error="failed to emit Typed U32 compare";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op==spv::OpLogicalOr) {
                    if (count!=5) { error="invalid logical-or instruction"; return false; }
                    const auto lhs=values.find(args[2]);
                    const auto rhs=values.find(args[3]);
                    if (lhs==values.end() || rhs==values.end() ||
                        lhs->second.kind()!=backend::TypedValueKind::Predicate ||
                        rhs->second.kind()!=backend::TypedValueKind::Predicate) {
                        error="logical-or operands are not Typed predicates";
                        return false;
                    }
                    const auto dst=program.make_predicate();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::PredicateOr>(0,dst,lhs->second,rhs->second)) {
                        error="failed to emit Typed predicate OR";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op==spv::OpSLessThan) {
                    if (count!=5) { error="invalid signed integer compare instruction"; return false; }
                    auto resolve_s32=[&](uint32_t id,backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id);it!=values.end() && it->second.type()==backend::TypedType::S32) {
                            value=it->second;
                            return true;
                        }
                        const auto constant=constants.find(id);
                        if (constant==constants.end()) return false;
                        value=program.literal_s32(static_cast<int32_t>(constant->second));
                        return value.kind()!=backend::TypedValueKind::None;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (!resolve_s32(args[2],lhs) || !resolve_s32(args[3],rhs)) {
                        error="signed loop compare operands are unresolved or non-S32";
                        return false;
                    }
                    const auto dst=program.make_predicate();
                    if (dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::Compare>(
                            static_cast<uint8_t>(usse::CompareOp::Less),dst,lhs,rhs)) {
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
                        if (auto constant=constants.find(id); constant!=constants.end()) {
                            value=program.literal_f32(constant->second);
                            return value.kind()!=backend::TypedValueKind::None;
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
                    const uint8_t components=backend::typed_component_count(lhs->second.type());
                    if ((stage==backend::TypedStage::Vertex || structured_control) && components<4) {
                        std::array<backend::TypedValue,3> products{};
                        bool ok=true;
                        for (uint8_t lane=0;lane<components;++lane) {
                            const auto a=program.make_value<backend::TypedType::F32>();
                            const auto b=program.make_value<backend::TypedType::F32>();
                            products[lane]=program.make_value<backend::TypedType::F32>();
                            ok = ok && a.kind()!=backend::TypedValueKind::None && b.kind()!=backend::TypedValueKind::None &&
                                products[lane].kind()!=backend::TypedValueKind::None &&
                                program.emit<backend::TypedOpcode::FloatExtract>(lane,a,lhs->second) &&
                                program.emit<backend::TypedOpcode::FloatExtract>(lane,b,rhs->second) &&
                                program.emit<backend::TypedOpcode::FloatBinary>(
                                    static_cast<uint8_t>(backend::TypedFloatOp::Mul),products[lane],a,b);
                        }
                        backend::TypedValue sum=products[0];
                        for (uint8_t lane=1;ok && lane<components;++lane) {
                            const auto next=program.make_value<backend::TypedType::F32>();
                            ok = next.kind()!=backend::TypedValueKind::None &&
                                program.emit<backend::TypedOpcode::FloatBinary>(
                                    static_cast<uint8_t>(backend::TypedFloatOp::Add),next,sum,products[lane]);
                            sum=next;
                        }
                        if (!ok) { error="failed to lower narrow F32 dot to scalar arithmetic"; return false; }
                        values[args[1]]=sum;
                        offset+=count;
                        continue;
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
                } else if (op == spv::OpVectorTimesScalar) {
                    if (count!=5) { error="invalid vector-times-scalar instruction"; return false; }
                    const auto result_type=typed_type(compiler.get_type(args[0]));
                    const auto vector=values.find(args[2]);
                    backend::TypedValue scalar{};
                    if (auto it=values.find(args[3]);it!=values.end() && it->second.type()==backend::TypedType::F32)
                        scalar=it->second;
                    else if (auto constant=constants.find(args[3]);constant!=constants.end())
                        scalar=program.literal_f32(constant->second);
                    const uint8_t components=backend::typed_component_count(result_type);
                    if ((result_type!=backend::TypedType::F32x2 && result_type!=backend::TypedType::F32x3 &&
                         result_type!=backend::TypedType::F32x4) || vector==values.end() ||
                        vector->second.type()!=result_type || scalar.kind()==backend::TypedValueKind::None ||
                        scalar.type()!=backend::TypedType::F32 || components<2 || components>4) {
                        error="OpVectorTimesScalar is outside the validated F32 vector/scalar subset";
                        return false;
                    }
                    const auto splat=program.make_value(result_type);
                    const auto dst=program.make_value(result_type);
                    if (splat.kind()==backend::TypedValueKind::None || dst.kind()==backend::TypedValueKind::None ||
                        !program.emit<backend::TypedOpcode::FloatSplat>(0,splat,scalar) ||
                        !program.emit<backend::TypedOpcode::FloatBinary>(
                            static_cast<uint8_t>(backend::TypedFloatOp::Mul),dst,vector->second,splat)) {
                        error="failed to lower OpVectorTimesScalar through splat/mul";
                        return false;
                    }
                    values[args[1]]=dst;
                } else if (op == spv::OpFMul || op == spv::OpFAdd || op == spv::OpFSub ||
                           op == spv::OpFDiv || op == spv::OpFMod) {
                    if (count != 5) { error = "invalid floating binary instruction"; return false; }
                    const auto result_type = typed_type(compiler.get_type(args[0]));
                    auto resolve_float=[&](uint32_t id, backend::TypedValue &value) -> bool {
                        if (auto it=values.find(id); it!=values.end() && it->second.type()==result_type) {
                            value=it->second;
                            return true;
                        }
                        if (result_type==backend::TypedType::F32) {
                            const auto constant=constants.find(id);
                            if (constant==constants.end()) return false;
                            value=program.literal_f32(constant->second);
                            return value.kind()!=backend::TypedValueKind::None;
                        }
                        const auto vector_constant=float_vector_constants.find(id);
                        const uint8_t components=backend::typed_component_count(result_type);
                        if (vector_constant==float_vector_constants.end() || components<2 || components>4)
                            return false;
                        if (result_type==backend::TypedType::F32x3) {
                            std::array<backend::TypedValue,3> lanes{};
                            for (uint8_t lane=0;lane<3;++lane) {
                                lanes[lane]=program.literal_f32(vector_constant->second[lane]);
                                if (lanes[lane].kind()==backend::TypedValueKind::None) return false;
                            }
                            const auto composed=program.compose_f32x3(lanes);
                            if (composed.kind()==backend::TypedValueKind::None) return false;
                            value=composed;
                            values[id]=composed;
                            return true;
                        }
                        for (uint8_t lane=1;lane<components;++lane)
                            if (vector_constant->second[lane]!=vector_constant->second[0]) return false;
                        const auto scalar=program.literal_f32(vector_constant->second[0]);
                        const auto splat=program.make_value(result_type);
                        if (scalar.kind()==backend::TypedValueKind::None || splat.kind()==backend::TypedValueKind::None ||
                            !program.emit<backend::TypedOpcode::FloatSplat>(0,splat,scalar))
                            return false;
                        value=splat;
                        values[id]=splat;
                        return true;
                    };
                    backend::TypedValue lhs{},rhs{};
                    if (result_type == backend::TypedType::Invalid || !backend::typed_is_float(result_type) ||
                        !resolve_float(args[2],lhs) || !resolve_float(args[3],rhs)) {
                        error = "floating binary operands are unresolved or mismatched (opcode " +
                            std::to_string(op) + ", lhs " + std::to_string(args[2]) +
                            ", rhs " + std::to_string(args[3]) + ")";
                        return false;
                    }
                    if (op==spv::OpFMod) {
                        if (result_type!=backend::TypedType::F32) {
                            error="OpFMod is outside the validated scalar F32 subset";
                            return false;
                        }
                        const auto quotient=program.make_value<backend::TypedType::F32>();
                        const auto floored=program.make_value<backend::TypedType::F32>();
                        const auto multiple=program.make_value<backend::TypedType::F32>();
                        const auto dst=program.make_value<backend::TypedType::F32>();
                        if (quotient.kind()==backend::TypedValueKind::None || floored.kind()==backend::TypedValueKind::None ||
                            multiple.kind()==backend::TypedValueKind::None || dst.kind()==backend::TypedValueKind::None ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Div),quotient,lhs,rhs) ||
                            !program.emit<backend::TypedOpcode::FloatUnary>(static_cast<uint8_t>(backend::TypedFloatUnaryOp::Floor),floored,quotient) ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Mul),multiple,floored,rhs) ||
                            !program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(backend::TypedFloatOp::Sub),dst,lhs,multiple)) {
                            error="failed to desugar scalar OpFMod";
                            return false;
                        }
                        values[args[1]]=dst;
                        offset+=count;
                        continue;
                    }
                    if ((stage==backend::TypedStage::Vertex || structured_control) && op==spv::OpFDiv &&
                        result_type==backend::TypedType::F32x3) {
                        std::array<backend::TypedValue,3> lanes{};
                        bool ok=true;
                        for (uint8_t lane=0;lane<3;++lane) {
                            const auto a=program.make_value<backend::TypedType::F32>();
                            const auto b=program.make_value<backend::TypedType::F32>();
                            lanes[lane]=program.make_value<backend::TypedType::F32>();
                            ok = ok && a.kind()!=backend::TypedValueKind::None && b.kind()!=backend::TypedValueKind::None &&
                                lanes[lane].kind()!=backend::TypedValueKind::None &&
                                program.emit<backend::TypedOpcode::FloatExtract>(lane,a,lhs) &&
                                program.emit<backend::TypedOpcode::FloatExtract>(lane,b,rhs) &&
                                program.emit<backend::TypedOpcode::FloatBinary>(
                                    static_cast<uint8_t>(backend::TypedFloatOp::Div),lanes[lane],a,b);
                        }
                        const auto composed=ok?program.compose_f32x3(lanes):backend::TypedValue{};
                        if (!ok || composed.kind()==backend::TypedValueKind::None) {
                            error="failed to lower vertex F32x3 division to scalar lanes";
                            return false;
                        }
                        values[args[1]]=composed;
                        offset+=count;
                        continue;
                    }
                    backend::TypedFloatOp float_op = backend::TypedFloatOp::Mul;
                    if (op == spv::OpFAdd) float_op = backend::TypedFloatOp::Add;
                    else if (op == spv::OpFSub) float_op = backend::TypedFloatOp::Sub;
                    else if (op == spv::OpFDiv) float_op = backend::TypedFloatOp::Div;
                    const auto dst = program.make_value(result_type);
                    if (!program.emit<backend::TypedOpcode::FloatBinary>(static_cast<uint8_t>(float_op), dst,
                                                                         lhs, rhs)) {
                        error = "failed to emit Typed IR float binary operation"; return false;
                    }
                    values[args[1]] = dst;
                } else if (op == spv::OpLogicalAnd) {
                    if (count!=5) { error="invalid logical-and instruction"; return false; }
                    const auto lhs=values.find(args[2]);
                    const auto rhs=values.find(args[3]);
                    if (lhs==values.end() || rhs==values.end() ||
                        lhs->second.kind()!=backend::TypedValueKind::Predicate ||
                        rhs->second.kind()!=backend::TypedValueKind::Predicate) {
                        error="logical-and operands are not Typed predicates";
                        return false;
                    }
                    logical_ands[args[1]]={lhs->second,rhs->second};
                } else if (op == spv::OpSelectionMerge) {
                    // Structured merge metadata is consumed by the control pre-pass.
                } else if (op == spv::OpBranchConditional) {
                    if (count!=4 || block_labels.empty()) { error="conditional branch is outside structured Typed shader subset"; return false; }
                    const auto true_label=block_labels.find(args[1]);
                    const auto false_label=block_labels.find(args[2]);
                    const bool true_kills=kill_labels.count(args[1])!=0;
                    const bool false_kills=kill_labels.count(args[2])!=0;
                    const auto logical_and=logical_ands.find(args[0]);
                    if (logical_and!=logical_ands.end()) {
                        const auto lhs=logical_and->second[0];
                        const auto rhs=logical_and->second[1];
                        const auto not_lhs=backend::TypedValue::predicate(lhs.id(),!lhs.inverted());
                        if (true_label==block_labels.end() || false_label==block_labels.end()) {
                            error="logical-and branch target is unresolved";
                            return false;
                        }
                        if (true_kills && !false_kills) {
                            if (!program.branch(false_label->second,not_lhs) ||
                                !program.emit<backend::TypedOpcode::Discard>(0,{},rhs) ||
                                !program.jump(false_label->second)) {
                                error="failed to sink logical-and discard into Typed control flow";
                                return false;
                            }
                        } else if (!true_kills && !false_kills) {
                            if (!program.branch(false_label->second,not_lhs) ||
                                !program.branch(true_label->second,rhs) || !program.jump(false_label->second)) {
                                error="failed to desugar logical-and branch into Typed control flow";
                                return false;
                            }
                        } else {
                            error="logical-and discard shape is outside the validated true-kill subset";
                            return false;
                        }
                    } else {
                        const auto predicate=values.find(args[0]);
                        if (predicate==values.end() || predicate->second.kind()!=backend::TypedValueKind::Predicate ||
                            true_label==block_labels.end() || false_label==block_labels.end()) {
                            error="conditional Typed branch predicate/target is unresolved";
                            return false;
                        }
                        if (true_kills != false_kills) {
                            auto discard_predicate=predicate->second;
                            const uint16_t survivor=true_kills ? false_label->second : true_label->second;
                            if (false_kills)
                                discard_predicate=backend::TypedValue::predicate(discard_predicate.id(),!discard_predicate.inverted());
                            if (!program.emit<backend::TypedOpcode::Discard>(0,{},discard_predicate) ||
                                !program.jump(survivor)) {
                                error="failed to sink branch-local discard into Typed IR";
                                return false;
                            }
                        } else if (!true_kills) {
                            if (!program.branch(true_label->second,predicate->second) || !program.jump(false_label->second)) {
                                error="failed to emit Typed shader conditional control flow";
                                return false;
                            }
                        } else {
                            error="both conditional targets kill; unsupported discard shape";
                            return false;
                        }
                    }
                    if (true_label==block_labels.end() || false_label==block_labels.end()) {
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
                            } else if (phi.type==backend::TypedType::F32x4) {
                                auto constant=float_vector_constants.find(incoming);
                                if (constant!=float_vector_constants.end())
                                    initial=program.literal_f32x4(constant->second);
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
                    // Internal two-way float4 phis (for example the nested
                    // lighting accumulators in vitaGL's fixed-function path)
                    // become one mutable Typed state value. Each predecessor
                    // writes the state before branching to the merge block;
                    // the OpPhi itself then aliases that stable handle.
                    for (auto &entry:phi_sinks) {
                        auto &phi=entry.second;
                        if (phi.loop_state || phi.output_resource!=std::numeric_limits<uint16_t>::max() ||
                            phi.merge_label!=target_id) continue;
                        if (phi.type!=backend::TypedType::F32x4) {
                            error="internal structured phi is outside the validated F32x4 subset";
                            return false;
                        }
                        uint32_t incoming=0;
                        if (phi.label0==current_label) incoming=phi.value0;
                        else if (phi.label1==current_label) incoming=phi.value1;
                        else {
                            error="internal phi has no incoming value for predecessor";
                            return false;
                        }
                        backend::TypedValue incoming_value{};
                        if (auto value=values.find(incoming);value!=values.end()) {
                            incoming_value=value->second;
                        } else if (auto constant=float_vector_constants.find(incoming);
                                   constant!=float_vector_constants.end()) {
                            incoming_value=program.literal_f32x4(constant->second);
                        }
                        if (incoming_value.kind()==backend::TypedValueKind::None ||
                            incoming_value.type()!=backend::TypedType::F32x4) {
                            error="internal float4 phi incoming value is unresolved";
                            return false;
                        }
                        auto state=values.find(entry.first);
                        if (state==values.end()) {
                            const auto mutable_value=program.make_value<backend::TypedType::F32x4>();
                            if (mutable_value.kind()==backend::TypedValueKind::None ||
                                !program.emit<backend::TypedOpcode::StateInit>(0,mutable_value,incoming_value)) {
                                error="failed to initialize internal float4 phi state";
                                return false;
                            }
                            values[entry.first]=mutable_value;
                        } else if (!program.emit<backend::TypedOpcode::StateUpdate>(0,state->second,incoming_value)) {
                            error="failed to update internal float4 phi state";
                            return false;
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
                    } else if (values.find(args[1])!=values.end()) {
                        // Internal structured phi state was materialized on its
                        // predecessor branches above.
                    } else if (phi->second.output_resource==std::numeric_limits<uint16_t>::max()) {
                        error="OpPhi is not the validated two-way fragment-output merge";
                        return false;
                    }
                    // The corresponding stores were sunk into the predecessor blocks.
                } else if (op == spv::OpSelect) {
                    const auto select=select_sinks.find(args[1]);
                    if (select==select_sinks.end() && stage==backend::TypedStage::Vertex) {
                        const auto result_type=typed_type(compiler.get_type(args[0]));
                        const auto predicate=values.find(args[2]);
                        auto resolve_scalar=[&](uint32_t id, backend::TypedValue &value) -> bool {
                            if (auto it=values.find(id);it!=values.end() && it->second.type()==backend::TypedType::F32) {
                                value=it->second;
                                return true;
                            }
                            if (auto constant=constants.find(id);constant!=constants.end()) {
                                value=program.literal_f32(constant->second);
                                return value.kind()!=backend::TypedValueKind::None;
                            }
                            return false;
                        };
                        backend::TypedValue true_value{},false_value{};
                        if (result_type!=backend::TypedType::F32 || predicate==values.end() ||
                            predicate->second.kind()!=backend::TypedValueKind::Predicate ||
                            !resolve_scalar(args[3],true_value) || !resolve_scalar(args[4],false_value)) {
                            error="vertex OpSelect is outside the validated scalar F32 subset";
                            return false;
                        }
                        const auto selected=program.select_f32(predicate->second,true_value,false_value);
                        if (selected.kind()==backend::TypedValueKind::None) {
                            error="failed to emit Typed scalar vertex select";
                            return false;
                        }
                        values[args[1]]=selected;
                        offset+=count;
                        continue;
                    }
                    if (select==select_sinks.end()) {
                        error="OpSelect was not recorded by the structured pre-scan";
                        return false;
                    }
                    if (select->second.output_resource==std::numeric_limits<uint16_t>::max()) {
                        const auto result_type=typed_type(compiler.get_type(args[0]));
                        const auto predicate=values.find(select->second.condition);
                        auto resolve_scalar=[&](uint32_t id, backend::TypedValue &value) -> bool {
                            if (auto it=values.find(id);it!=values.end() && it->second.type()==backend::TypedType::F32) {
                                value=it->second;
                                return true;
                            }
                            if (auto constant=constants.find(id);constant!=constants.end()) {
                                value=program.literal_f32(constant->second);
                                return value.kind()!=backend::TypedValueKind::None;
                            }
                            return false;
                        };
                        backend::TypedValue true_value{},false_value{};
                        if (result_type!=backend::TypedType::F32 || predicate==values.end() ||
                            predicate->second.kind()!=backend::TypedValueKind::Predicate ||
                            !resolve_scalar(select->second.true_value,true_value) ||
                            !resolve_scalar(select->second.false_value,false_value)) {
                            error="non-output OpSelect is outside the validated scalar F32 subset";
                            return false;
                        }
                        const auto selected=program.select_f32(predicate->second,true_value,false_value);
                        if (selected.kind()==backend::TypedValueKind::None) {
                            error="failed to emit Typed scalar F32 select";
                            return false;
                        }
                        values[args[1]]=selected;
                        offset+=count;
                        continue;
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
                    if (const auto state=function_array_one_accesses.find(args[0]);state!=function_array_one_accesses.end()) {
                        const auto value=values.find(args[1]);
                        if (value==values.end() || value->second.type()!=backend::TypedType::F32 ||
                            !program.emit<backend::TypedOpcode::StateUpdate>(0,state->second,value->second)) {
                            error="failed to update scalar one-element function array";
                            return false;
                        }
                        offset+=count;
                        continue;
                    }
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
                } else if (op==spv::OpKill) {
                    // Branch-local kills are sunk into their predecessor so the
                    // compact Typed CFG only contains the surviving path.
                } else if (op != spv::OpReturn && op != spv::OpNop) {
                    error = "unsupported instruction in SPIRV-Cross Typed shader subset (opcode " +
                        std::to_string(op) + ")";
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
