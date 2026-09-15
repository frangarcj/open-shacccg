#pragma once

#include "gxp/gxp_reader.hpp"

#include <cstddef>
#include <cstdint>

namespace vsc::gxp {

struct ParameterDesc {
    const char *name = nullptr;
    uint8_t category = 0;
    uint8_t type = 0;
    uint8_t component_count = 0;
    uint8_t container_index = 0;
    uint8_t semantic = 0;
    uint8_t semantic_index = 0;
    uint32_t array_size = 1;
    uint32_t resource_index = 0;
};

struct ParameterContainerDesc {
    uint16_t container_index = 0;
    uint16_t unknown = 0;
    uint16_t base_sa_offset = 0;
    uint16_t size_in_f32 = 0;
};

// Observed v1.4 literal-table entry. `resource_index` selects the scalar
// secondary-attribute slot within container 19 and `value_bits` is the raw
// 32-bit payload consumed by USSE. The oracle anchors both integer 0/1 and
// packed floating-point constants to this exact 8-byte layout.
struct LiteralDesc {
    uint32_t resource_index = 0;
    uint32_t value_bits = 0;
};
static_assert(sizeof(LiteralDesc) == 8, "GXP literal entries must stay 8 bytes");

// Minimal, canonical GXP serializer input.  It covers the pieces needed by the
// first backend milestones (interface block, code, uniform/sampler reflection
// and parameter containers). More exotic auxiliary tables are intentionally
// added only after observed test cases require them.
struct ProgramImage {
    ProgramType type = ProgramType::Vertex;
    uint8_t major_version = 1;
    uint8_t minor_version = 4;
    uint16_t sdk_version = 0;
    uint32_t binary_guid = 0;
    uint32_t source_guid = 0;
    uint32_t program_flags = 0;
    uint32_t buffer_flags = 0;
    uint32_t texunit_flags[2] = {0, 0};

    uint16_t primary_register_count = 0;
    uint16_t secondary_register_count = 0;
    uint32_t temp_register_count = 0;
    uint16_t temp_register_count_phase2 = 0;
    uint16_t primary_phase_count = 1;

    uint32_t scratch_buffer_count = 0;
    uint32_t thread_buffer_count = 0;
    uint32_t literal_buffer_count = 0;
    uint32_t data_buffer_count = 0;
    uint32_t texture_buffer_count = 0;
    uint32_t default_uniform_buffer_count = 0;
    uint32_t compiler_version_raw = 4;

    // Exactly 32 bytes when supplied. A null pointer produces a zeroed block.
    const uint8_t *interface_block = nullptr;
    size_t interface_block_size = 0;

    // Some fragment programs without a secondary stream use the reserved
    // 8-byte slot after the 32-byte interface record for dependent-sampler
    // metadata. Null means the canonical all-zero slot.
    const uint8_t *fragment_interface_extension = nullptr;
    size_t fragment_interface_extension_size = 0;

    // Oracle-observed sampler/control layout with a secondary stream: one
    // 32-bit interface word follows the canonical 32-byte block, then secondary
    // code begins immediately. Zero keeps the older in-interface secondary
    // placement used by scalar/integer profiles.
    uint32_t fragment_secondary_prefix_word = 0;

    // SDK 1.6.5 no-secondary fragment programs place one 16-byte descriptor
    // after the main 32-byte interface for each additional input beyond
    // Location 0, followed by the usual 8-byte secondary anchor slot. Current
    // oracle coverage anchors homogeneous F32 widths and up to Locations 1/2.
    uint8_t fragment_additional_inputs = 0;
    uint8_t fragment_input_components = 4;

    // Oracle-observed constant-fragment layout: the first primary qword starts
    // at interface+0x18 and therefore occupies the final 8 bytes of the
    // 32-byte interface record. This is distinct from the normal no-secondary
    // fragment layout, which reserves an 8-byte extension slot first.
    bool fragment_primary_overlaps_interface = false;

    // Oracle-observed SDK 1.6.5 vertex layout for the compact standalone
    // profiles: reserve one 32-bit word after the interface before primary
    // code. Public libvita2d vertex GXPs use the older no-gap convention, so
    // this remains opt-in per profile.
    bool vertex_primary_padding_word = false;

    const uint64_t *secondary_instructions = nullptr;
    size_t secondary_instruction_count = 0;
    const uint64_t *primary_instructions = nullptr;
    size_t primary_instruction_count = 0;

    const ParameterContainerDesc *containers = nullptr;
    size_t container_count = 0;
    const ParameterDesc *parameters = nullptr;
    size_t parameter_count = 0;
    const LiteralDesc *literals = nullptr;
    size_t literal_count = 0;
};

struct WriteResult {
    size_t logical_size = 0;
    size_t physical_size = 0;
};

// Returns zero if the description cannot be represented by the currently
// supported canonical layout.
size_t required_size(const ProgramImage &image);

// Serializes a canonical GXP image into caller-owned storage. The serializer
// is structurally round-trip tested; acceptance by real SceGxm is a separate
// hardware-validation milestone and is not claimed until that test is run.
bool write_program(const ProgramImage &image, uint8_t *output, size_t capacity,
                   WriteResult *result = nullptr);

} // namespace vsc::gxp
