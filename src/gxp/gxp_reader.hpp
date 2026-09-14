#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace vsc::gxp {

enum class ProgramType : uint8_t {
    Vertex = 0,
    Fragment = 1,
};

struct ByteRange {
    const uint8_t *data = nullptr;
    size_t size = 0;
};

struct ParameterView {
    std::string_view name{};
    uint8_t category = 0;
    uint8_t type = 0;
    uint8_t component_count = 0;
    uint8_t container_index = 0;
    uint8_t semantic = 0;
    uint8_t semantic_index = 0;
    uint32_t array_size = 0;
    uint32_t resource_index = 0;
};

// Read-only view over a serialized SceGxmProgram/GXP image.
//
// The reader intentionally uses byte offsets rather than mirroring Sony or
// emulator C++ structs. The layout represented here was independently checked
// against working, MIT-licensed GXP binaries and the observable SceGxm API.
class ProgramView {
public:
    static constexpr size_t kHeaderSize = 0x9c;
    static constexpr size_t kParameterSize = 0x10;
    static constexpr size_t kInstructionSize = 8;

    ProgramView(const void *data, size_t size);

    bool valid() const;
    const char *error() const { return error_; }

    uint8_t major_version() const;
    uint8_t minor_version() const;
    uint16_t sdk_version() const;
    uint32_t logical_size() const;
    uint32_t binary_guid() const;
    uint32_t source_guid() const;
    uint32_t flags() const;
    ProgramType type() const;

    uint32_t parameter_count() const;
    bool parameter(uint32_t index, ParameterView &out) const;

    uint16_t primary_register_count() const;
    uint16_t secondary_register_count() const;
    uint32_t primary_instruction_count() const;
    uint32_t secondary_instruction_count() const;
    ByteRange primary_program() const;
    ByteRange secondary_program() const;
    ByteRange varyings() const;

    uint32_t compiler_version_raw() const;
    uint32_t literal_count() const;
    uint32_t uniform_buffer_count() const;
    uint32_t dependent_sampler_count() const;
    uint32_t container_count() const;

private:
    uint8_t u8(size_t offset) const;
    uint16_t u16(size_t offset) const;
    uint32_t u32(size_t offset) const;
    int32_t i32(size_t offset) const;
    bool self_relative_range(size_t field_offset, uint32_t encoded_offset,
                             size_t byte_count, ByteRange &out) const;
    bool cstring_from(size_t offset, std::string_view &out) const;
    size_t checked_limit() const;
    void validate();

    const uint8_t *data_ = nullptr;
    size_t size_ = 0;
    const char *error_ = nullptr;
};

} // namespace vsc::gxp
