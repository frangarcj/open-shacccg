#include "gxp/gxp_writer.hpp"
#include "core/compact_layout.hpp"

#include <cstring>
#include <limits>

namespace vsc::gxp {
namespace {
constexpr uint32_t kMagic = 0x00505847u;
constexpr uint32_t kFragmentFlag = 1u;
constexpr size_t kFixedPrefixSize = 0x98;
constexpr size_t kInterfaceSize = 32;
constexpr size_t kContainerSize = 8;
constexpr size_t kLiteralSize = 8;

constexpr size_t kOffMagic = 0x00;
constexpr size_t kOffMajor = 0x04;
constexpr size_t kOffMinor = 0x05;
constexpr size_t kOffSdk = 0x06;
constexpr size_t kOffSize = 0x08;
constexpr size_t kOffBinaryGuid = 0x0c;
constexpr size_t kOffSourceGuid = 0x10;
constexpr size_t kOffFlags = 0x14;
constexpr size_t kOffBufferFlags = 0x18;
constexpr size_t kOffTexunit0 = 0x1c;
constexpr size_t kOffTexunit1 = 0x20;
constexpr size_t kOffParameterCount = 0x24;
constexpr size_t kOffParameters = 0x28;
constexpr size_t kOffVaryings = 0x2c;
constexpr size_t kOffPrimaryRegs = 0x30;
constexpr size_t kOffSecondaryRegs = 0x32;
constexpr size_t kOffTemp1 = 0x34;
constexpr size_t kOffTemp2 = 0x38;
constexpr size_t kOffPhaseCount = 0x3a;
constexpr size_t kOffPrimaryInstrCount = 0x3c;
constexpr size_t kOffPrimaryProgram = 0x40;
constexpr size_t kOffSecondaryInstrCount = 0x44;
constexpr size_t kOffSecondaryProgram = 0x48;
constexpr size_t kOffSecondaryEnd = 0x4c;
constexpr size_t kOffScratchCount = 0x50;
constexpr size_t kOffThreadCount = 0x54;
constexpr size_t kOffLiteralBufferCount = 0x58;
constexpr size_t kOffDataBufferCount = 0x5c;
constexpr size_t kOffTextureBufferCount = 0x60;
constexpr size_t kOffDefaultUniformCount = 0x64;
constexpr size_t kOffLiteralData = 0x68;
constexpr size_t kOffCompilerVersion = 0x6c;
constexpr size_t kOffLiteralsCount = 0x70;
constexpr size_t kOffLiterals = 0x74;
constexpr size_t kOffUniformBufferCount = 0x78;
constexpr size_t kOffUniformBuffers = 0x7c;
constexpr size_t kOffDependentSamplerCount = 0x80;
constexpr size_t kOffDependentSamplers = 0x84;
constexpr size_t kOffTextureDependentSamplerCount = 0x88;
constexpr size_t kOffTextureDependentSamplers = 0x8c;
constexpr size_t kOffContainerCount = 0x90;
constexpr size_t kOffContainers = 0x94;

size_t align_up(size_t value, size_t alignment) {
    if (!alignment) return value;
    const size_t mask = alignment - 1;
    if (value > std::numeric_limits<size_t>::max() - mask) return 0;
    return (value + mask) & ~mask;
}

bool add_size(size_t &value, size_t amount) {
    if (amount > std::numeric_limits<size_t>::max() - value) return false;
    value += amount;
    return true;
}

bool mul_size(size_t a, size_t b, size_t &out) {
    if (a && b > std::numeric_limits<size_t>::max() / a) return false;
    out = a * b;
    return true;
}

size_t string_bytes(const ProgramImage &image) {
    size_t total = 0;
    for (size_t i = 0; i < image.parameter_count; ++i) {
        if (!image.parameters || !image.parameters[i].name) return 0;
        const size_t len = std::strlen(image.parameters[i].name) + 1;
        if (!add_size(total, len)) return 0;
    }
    return total;
}

bool valid_desc(const ProgramImage &image) {
    if (image.interface_block && image.interface_block_size != kInterfaceSize)
        return false;
    if (!image.interface_block && image.interface_block_size != 0)
        return false;
    if (image.fragment_interface_extension && image.fragment_interface_extension_size != 8)
        return false;
    if (!image.fragment_interface_extension && image.fragment_interface_extension_size != 0)
        return false;
    if (image.fragment_interface_extension && image.type != ProgramType::Fragment)
        return false;
    if (image.primary_instruction_count && !image.primary_instructions)
        return false;
    if (image.secondary_instruction_count && !image.secondary_instructions)
        return false;
    if (image.container_count && !image.containers)
        return false;
    if (image.parameter_count && !image.parameters)
        return false;
    if (image.literal_count && !image.literals)
        return false;
    if (image.primary_instruction_count > std::numeric_limits<uint32_t>::max() ||
        image.secondary_instruction_count > std::numeric_limits<uint32_t>::max() ||
        image.container_count > std::numeric_limits<uint32_t>::max() ||
        image.parameter_count > std::numeric_limits<uint32_t>::max() ||
        image.literal_count > std::numeric_limits<uint32_t>::max())
        return false;
    for (size_t i = 0; i < image.parameter_count; ++i) {
        if (!image.parameters[i].name) return false;
        if (image.parameters[i].category > 0x0f || image.parameters[i].type > 0x0f ||
            image.parameters[i].component_count > 0x0f || image.parameters[i].container_index > 0x0f)
            return false;
    }
    return true;
}

struct Layout {
    size_t interface_off = 0;
    size_t secondary_off = 0;
    size_t secondary_end = 0;
    size_t primary_off = 0;
    size_t literals_off = 0;
    size_t aux_off = 0;
    size_t containers_off = 0;
    size_t parameters_off = 0;
    size_t strings_off = 0;
    size_t logical_size = 0;
    size_t physical_size = 0;
};

bool compute_layout(const ProgramImage &image, Layout &l) {
    if (!valid_desc(image)) return false;
    size_t cursor = kFixedPrefixSize;
    l.interface_off = cursor;
    if (!add_size(cursor, kInterfaceSize)) return false;
    cursor = align_up(cursor, 8); if (!cursor) return false;

    size_t bytes = 0;
    if (image.type == ProgramType::Fragment && image.secondary_instruction_count) {
        // In the validated clear_f program the single secondary USSE2 word is
        // embedded in the final 12 bytes of the 32-byte fragment interface
        // record.  The primary stream still starts immediately after the
        // interface record, so the embedded secondary word must not advance
        // the main layout cursor.  Keep this fail-closed until a public sample
        // demonstrates a larger fragment secondary stream.
        if (image.secondary_instruction_count != 1) return false;
        l.secondary_off = l.interface_off + 20;
        l.secondary_end = l.secondary_off + sizeof(uint64_t);
    } else {
        // Known-good v1.4 programs with no secondary stream use the word
        // immediately before the primary stream as the canonical anchor.
        // Fragment programs with no secondary stream additionally reserve one
        // 64-bit slot after the interface record: color_f/texture_f therefore
        // begin primary code at 0xC0, unlike clear_f whose embedded secondary
        // instruction allows primary code to begin at 0xB8.
        if (image.type == ProgramType::Fragment && image.secondary_instruction_count == 0) {
            if (!add_size(cursor, sizeof(uint64_t))) return false;
        }
        l.secondary_off = image.secondary_instruction_count ? cursor : cursor - 4;
        if (!mul_size(image.secondary_instruction_count, sizeof(uint64_t), bytes) || !add_size(cursor, bytes)) return false;
        l.secondary_end = image.secondary_instruction_count ? cursor : l.secondary_off;
    }

    l.primary_off = cursor;
    if (!mul_size(image.primary_instruction_count, sizeof(uint64_t), bytes) || !add_size(cursor, bytes)) return false;
    cursor = align_up(cursor, 4); if (!cursor) return false;

    l.literals_off = cursor;
    if (!mul_size(image.literal_count, kLiteralSize, bytes) || !add_size(cursor, bytes)) return false;
    l.aux_off = cursor;

    l.containers_off = cursor;
    if (!mul_size(image.container_count, kContainerSize, bytes) || !add_size(cursor, bytes)) return false;

    l.parameters_off = cursor;
    if (!mul_size(image.parameter_count, ProgramView::kParameterSize, bytes) || !add_size(cursor, bytes)) return false;
    l.strings_off = cursor;
    const size_t names = string_bytes(image);
    if (image.parameter_count && !names) return false;
    if (!add_size(cursor, names)) return false;
    l.logical_size = cursor;
    l.physical_size = align_up(cursor, 4);
    return l.physical_size != 0 && l.logical_size <= std::numeric_limits<uint32_t>::max();
}

bool put_rel32(uint8_t *out, size_t field, size_t target) {
    if (target < field || target - field > std::numeric_limits<uint32_t>::max()) return false;
    binary::store<uint32_t>(out, field, static_cast<uint32_t>(target - field));
    return true;
}

void write_header_fields(uint8_t *out, const ProgramImage &image) {
#define VSC_GXP_FIELD(type, member, offset) \
    binary::MemberField<type, &ProgramImage::member, offset>::write(out, 0, image);
#define VSC_GXP_ARRAY_FIELD(type, member, index, offset) \
    binary::ArrayField<type, &ProgramImage::member, index, offset>::write(out, 0, image);
#include "gxp/header_fields.inc"
#undef VSC_GXP_ARRAY_FIELD
#undef VSC_GXP_FIELD
}

} // namespace

size_t required_size(const ProgramImage &image) {
    Layout layout;
    return compute_layout(image, layout) ? layout.physical_size : 0;
}

bool write_program(const ProgramImage &image, uint8_t *output, size_t capacity,
                   WriteResult *result) {
    Layout l;
    if (!compute_layout(image, l) || !output || capacity < l.physical_size)
        return false;

    std::memset(output, 0, l.physical_size);
    binary::store<uint32_t>(output, kOffMagic, kMagic);
    write_header_fields(output, image);
    binary::store<uint32_t>(output, kOffSize, static_cast<uint32_t>(l.logical_size));
    uint32_t flags = image.program_flags & ~kFragmentFlag;
    if (image.type == ProgramType::Fragment) flags |= kFragmentFlag;
    binary::store<uint32_t>(output, kOffFlags, flags);
    if (!put_rel32(output, kOffParameters, l.parameters_off) ||
        !put_rel32(output, kOffVaryings, l.interface_off)) return false;

    if (!put_rel32(output, kOffPrimaryProgram, l.primary_off)) return false;
    if (!put_rel32(output, kOffSecondaryProgram, l.secondary_off) ||
        !put_rel32(output, kOffSecondaryEnd, l.secondary_end)) return false;

    if (!put_rel32(output, kOffLiteralData, l.aux_off)) return false;

    // Literal entries are the first validated auxiliary table. `literalData`
    // points immediately after that table in Sony's v1.4 images; when the
    // table is empty both relative pointers naturally collapse to aux_off,
    // preserving all existing byte-identical public regressions.
    binary::store<uint32_t>(output, kOffLiteralsCount, static_cast<uint32_t>(image.literal_count));
    if (!put_rel32(output, kOffLiterals, l.literals_off)) return false;
    binary::store<uint32_t>(output, kOffUniformBufferCount, 0);
    if (!put_rel32(output, kOffUniformBuffers, l.parameters_off)) return false;
    binary::store<uint32_t>(output, kOffDependentSamplerCount, 0);
    if (!put_rel32(output, kOffDependentSamplers, l.aux_off)) return false;
    binary::store<uint32_t>(output, kOffTextureDependentSamplerCount, 0);
    if (!put_rel32(output, kOffTextureDependentSamplers, l.aux_off)) return false;
    if (!put_rel32(output, kOffContainers, l.containers_off)) return false;

    if (image.interface_block)
        std::memcpy(output + l.interface_off, image.interface_block, kInterfaceSize);
    if (image.fragment_interface_extension)
        std::memcpy(output + l.interface_off + kInterfaceSize,
                    image.fragment_interface_extension, 8);
    if (image.secondary_instruction_count)
        std::memcpy(output + l.secondary_off, image.secondary_instructions,
                    image.secondary_instruction_count * sizeof(uint64_t));
    if (image.primary_instruction_count)
        std::memcpy(output + l.primary_off, image.primary_instructions,
                    image.primary_instruction_count * sizeof(uint64_t));

    for (size_t i = 0; i < image.literal_count; ++i) {
        const auto &literal = image.literals[i];
        const size_t off = l.literals_off + i * kLiteralSize;
        binary::store<uint32_t>(output, off, literal.resource_index);
        binary::store<uint32_t>(output, off + 4, literal.value_bits);
    }

    for (size_t i = 0; i < image.container_count; ++i) {
        const auto &c = image.containers[i];
        const size_t off = l.containers_off + i * kContainerSize;
        binary::write_fields<ParameterContainerDesc,
            binary::MemberField<uint16_t, &ParameterContainerDesc::container_index, 0>,
            binary::MemberField<uint16_t, &ParameterContainerDesc::unknown, 2>,
            binary::MemberField<uint16_t, &ParameterContainerDesc::base_sa_offset, 4>,
            binary::MemberField<uint16_t, &ParameterContainerDesc::size_in_f32, 6>
        >(output, off, c);
    }

    size_t name_cursor = l.strings_off;
    for (size_t i = 0; i < image.parameter_count; ++i) {
        const auto &p = image.parameters[i];
        const size_t off = l.parameters_off + i * ProgramView::kParameterSize;
        const int64_t delta = static_cast<int64_t>(name_cursor) - static_cast<int64_t>(off);
        if (delta < std::numeric_limits<int32_t>::min() || delta > std::numeric_limits<int32_t>::max())
            return false;
        binary::store<int32_t>(output, off, static_cast<int32_t>(delta));
        binary::write_fields<ParameterDesc,
            binary::NibbleField<&ParameterDesc::category, &ParameterDesc::type, 4>,
            binary::NibbleField<&ParameterDesc::component_count, &ParameterDesc::container_index, 5>,
            binary::MemberField<uint8_t, &ParameterDesc::semantic, 6>,
            binary::MemberField<uint8_t, &ParameterDesc::semantic_index, 7>,
            binary::MemberField<uint32_t, &ParameterDesc::array_size, 8>,
            binary::MemberField<uint32_t, &ParameterDesc::resource_index, 12>
        >(output, off, p);
        const size_t len = std::strlen(p.name) + 1;
        std::memcpy(output + name_cursor, p.name, len);
        name_cursor += len;
    }

    if (result) {
        result->logical_size = l.logical_size;
        result->physical_size = l.physical_size;
    }
    return true;
}

} // namespace vsc::gxp
