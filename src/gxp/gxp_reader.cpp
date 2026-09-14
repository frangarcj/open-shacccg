#include "gxp/gxp_reader.hpp"

#include <cstring>
#include <limits>

namespace vsc::gxp {
namespace {
constexpr uint32_t kMagic = 0x00505847u; // "GXP\0", little-endian
constexpr uint32_t kFragmentFlag = 1u;

constexpr size_t kOffMagic = 0x00;
constexpr size_t kOffMajor = 0x04;
constexpr size_t kOffMinor = 0x05;
constexpr size_t kOffSdk = 0x06;
constexpr size_t kOffSize = 0x08;
constexpr size_t kOffBinaryGuid = 0x0c;
constexpr size_t kOffSourceGuid = 0x10;
constexpr size_t kOffFlags = 0x14;
constexpr size_t kOffParameterCount = 0x24;
constexpr size_t kOffParameters = 0x28;
constexpr size_t kOffVaryings = 0x2c;
constexpr size_t kOffPrimaryRegs = 0x30;
constexpr size_t kOffSecondaryRegs = 0x32;
constexpr size_t kOffPrimaryInstrCount = 0x3c;
constexpr size_t kOffPrimaryProgram = 0x40;
constexpr size_t kOffSecondaryInstrCount = 0x44;
constexpr size_t kOffSecondaryProgram = 0x48;
constexpr size_t kOffCompilerVersion = 0x6c;
constexpr size_t kOffLiteralCount = 0x70;
constexpr size_t kOffUniformBufferCount = 0x78;
constexpr size_t kOffDependentSamplerCount = 0x80;
constexpr size_t kOffContainerCount = 0x90;
} // namespace

ProgramView::ProgramView(const void *data, size_t size)
    : data_(static_cast<const uint8_t *>(data)), size_(size) {
    validate();
}

uint8_t ProgramView::u8(size_t offset) const {
    return data_[offset];
}

uint16_t ProgramView::u16(size_t offset) const {
    uint16_t value;
    std::memcpy(&value, data_ + offset, sizeof(value));
    return value;
}

uint32_t ProgramView::u32(size_t offset) const {
    uint32_t value;
    std::memcpy(&value, data_ + offset, sizeof(value));
    return value;
}

int32_t ProgramView::i32(size_t offset) const {
    int32_t value;
    std::memcpy(&value, data_ + offset, sizeof(value));
    return value;
}

size_t ProgramView::checked_limit() const {
    if (!data_ || size_ < kHeaderSize)
        return 0;
    const uint32_t declared = u32(kOffSize);
    return declared <= size_ ? declared : 0;
}

bool ProgramView::self_relative_range(size_t field_offset, uint32_t encoded_offset,
                                      size_t byte_count, ByteRange &out) const {
    out = {};
    const size_t limit = checked_limit();
    if (!limit)
        return false;
    if (encoded_offset > std::numeric_limits<size_t>::max() - field_offset)
        return false;
    const size_t begin = field_offset + static_cast<size_t>(encoded_offset);
    if (begin > limit || byte_count > limit - begin)
        return false;
    out.data = data_ + begin;
    out.size = byte_count;
    return true;
}

bool ProgramView::cstring_from(size_t offset, std::string_view &out) const {
    out = {};
    const size_t limit = checked_limit();
    if (!limit || offset >= limit)
        return false;
    const void *end = std::memchr(data_ + offset, 0, limit - offset);
    if (!end)
        return false;
    const auto *end8 = static_cast<const uint8_t *>(end);
    out = std::string_view(reinterpret_cast<const char *>(data_ + offset),
                           static_cast<size_t>(end8 - (data_ + offset)));
    return true;
}

void ProgramView::validate() {
    if (!data_) {
        error_ = "null GXP image";
        return;
    }
    if (size_ < kHeaderSize) {
        error_ = "GXP image is smaller than the program header";
        return;
    }
    if (u32(kOffMagic) != kMagic) {
        error_ = "invalid GXP magic";
        return;
    }
    if (u8(kOffMajor) < 1) {
        error_ = "unsupported GXP major version";
        return;
    }
    const uint32_t declared = u32(kOffSize);
    if (declared < kHeaderSize) {
        error_ = "invalid GXP logical size";
        return;
    }
    if (declared > size_) {
        error_ = "truncated GXP image";
        return;
    }

    const uint32_t pcount = u32(kOffParameterCount);
    if (pcount > (std::numeric_limits<size_t>::max() / kParameterSize)) {
        error_ = "GXP parameter count overflows address space";
        return;
    }
    if (pcount) {
        ByteRange params;
        if (!self_relative_range(kOffParameters, u32(kOffParameters),
                                 static_cast<size_t>(pcount) * kParameterSize, params)) {
            error_ = "GXP parameter table is out of range";
            return;
        }
        for (uint32_t i = 0; i < pcount; ++i) {
            const size_t poff = static_cast<size_t>(params.data - data_) +
                                static_cast<size_t>(i) * kParameterSize;
            const int32_t name_delta = i32(poff);
            const int64_t name64 = static_cast<int64_t>(poff) + name_delta;
            if (name64 < 0 || static_cast<uint64_t>(name64) >= declared) {
                error_ = "GXP parameter name offset is out of range";
                return;
            }
            std::string_view ignored;
            if (!cstring_from(static_cast<size_t>(name64), ignored)) {
                error_ = "GXP parameter name is unterminated";
                return;
            }
        }
    }

    const uint32_t primary_count = u32(kOffPrimaryInstrCount);
    if (primary_count > (std::numeric_limits<size_t>::max() / kInstructionSize)) {
        error_ = "GXP primary instruction count overflows address space";
        return;
    }
    ByteRange primary;
    if (!self_relative_range(kOffPrimaryProgram, u32(kOffPrimaryProgram),
                             static_cast<size_t>(primary_count) * kInstructionSize, primary)) {
        error_ = "GXP primary program is out of range";
        return;
    }

    const uint32_t secondary_count = u32(kOffSecondaryInstrCount);
    if (secondary_count > (std::numeric_limits<size_t>::max() / kInstructionSize)) {
        error_ = "GXP secondary instruction count overflows address space";
        return;
    }
    if (secondary_count) {
        ByteRange secondary;
        if (!self_relative_range(kOffSecondaryProgram, u32(kOffSecondaryProgram),
                                 static_cast<size_t>(secondary_count) * kInstructionSize, secondary)) {
            error_ = "GXP secondary program is out of range";
            return;
        }
    }

    error_ = nullptr;
}

bool ProgramView::valid() const { return error_ == nullptr; }
uint8_t ProgramView::major_version() const { return valid() ? u8(kOffMajor) : 0; }
uint8_t ProgramView::minor_version() const { return valid() ? u8(kOffMinor) : 0; }
uint16_t ProgramView::sdk_version() const { return valid() ? u16(kOffSdk) : 0; }
uint32_t ProgramView::logical_size() const { return valid() ? u32(kOffSize) : 0; }
uint32_t ProgramView::binary_guid() const { return valid() ? u32(kOffBinaryGuid) : 0; }
uint32_t ProgramView::source_guid() const { return valid() ? u32(kOffSourceGuid) : 0; }
uint32_t ProgramView::flags() const { return valid() ? u32(kOffFlags) : 0; }
ProgramType ProgramView::type() const {
    return (flags() & kFragmentFlag) ? ProgramType::Fragment : ProgramType::Vertex;
}
uint32_t ProgramView::parameter_count() const { return valid() ? u32(kOffParameterCount) : 0; }
uint16_t ProgramView::primary_register_count() const { return valid() ? u16(kOffPrimaryRegs) : 0; }
uint16_t ProgramView::secondary_register_count() const { return valid() ? u16(kOffSecondaryRegs) : 0; }
uint32_t ProgramView::primary_instruction_count() const { return valid() ? u32(kOffPrimaryInstrCount) : 0; }
uint32_t ProgramView::secondary_instruction_count() const { return valid() ? u32(kOffSecondaryInstrCount) : 0; }
uint32_t ProgramView::compiler_version_raw() const { return valid() ? u32(kOffCompilerVersion) : 0; }
uint32_t ProgramView::literal_count() const { return valid() ? u32(kOffLiteralCount) : 0; }
uint32_t ProgramView::uniform_buffer_count() const { return valid() ? u32(kOffUniformBufferCount) : 0; }
uint32_t ProgramView::dependent_sampler_count() const { return valid() ? u32(kOffDependentSamplerCount) : 0; }
uint32_t ProgramView::container_count() const { return valid() ? u32(kOffContainerCount) : 0; }

ByteRange ProgramView::primary_program() const {
    ByteRange out;
    if (!valid()) return out;
    self_relative_range(kOffPrimaryProgram, u32(kOffPrimaryProgram),
                        static_cast<size_t>(primary_instruction_count()) * kInstructionSize, out);
    return out;
}

ByteRange ProgramView::secondary_program() const {
    ByteRange out;
    if (!valid() || !secondary_instruction_count()) return out;
    self_relative_range(kOffSecondaryProgram, u32(kOffSecondaryProgram),
                        static_cast<size_t>(secondary_instruction_count()) * kInstructionSize, out);
    return out;
}

ByteRange ProgramView::varyings() const {
    ByteRange out;
    if (!valid()) return out;
    // The fixed varying/interface block in GXP v1.4 is 32 bytes. Some future
    // revisions may differ; callers should treat it as opaque for now.
    self_relative_range(kOffVaryings, u32(kOffVaryings), 32, out);
    return out;
}

bool ProgramView::parameter(uint32_t index, ParameterView &out) const {
    out = {};
    if (!valid() || index >= parameter_count())
        return false;

    ByteRange params;
    if (!self_relative_range(kOffParameters, u32(kOffParameters),
                             static_cast<size_t>(parameter_count()) * kParameterSize, params))
        return false;

    const size_t poff = static_cast<size_t>(params.data - data_) +
                        static_cast<size_t>(index) * kParameterSize;
    const int32_t name_delta = i32(poff);
    const int64_t name64 = static_cast<int64_t>(poff) + name_delta;
    if (name64 < 0 || static_cast<uint64_t>(name64) >= checked_limit())
        return false;
    if (!cstring_from(static_cast<size_t>(name64), out.name))
        return false;

    const uint8_t packed_type = u8(poff + 4);
    const uint8_t packed_shape = u8(poff + 5);
    out.category = packed_type & 0x0f;
    out.type = packed_type >> 4;
    out.component_count = packed_shape & 0x0f;
    out.container_index = packed_shape >> 4;
    out.semantic = u8(poff + 6);
    out.semantic_index = u8(poff + 7);
    out.array_size = u32(poff + 8);
    out.resource_index = u32(poff + 12);
    return true;
}

} // namespace vsc::gxp
