#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace vsc::binary {

template <typename T>
inline void store(uint8_t *out, size_t offset, T value) {
    std::memcpy(out + offset, &value, sizeof(value));
}

template <typename Wire, auto Member, size_t Offset>
struct MemberField {
    template <typename Struct>
    static void write(uint8_t *out, size_t base, const Struct &source) {
        store<Wire>(out, base + Offset, static_cast<Wire>(source.*Member));
    }
};

template <typename Wire, auto Member, size_t Index, size_t Offset>
struct ArrayField {
    template <typename Struct>
    static void write(uint8_t *out, size_t base, const Struct &source) {
        store<Wire>(out, base + Offset, static_cast<Wire>((source.*Member)[Index]));
    }
};

template <auto LowMember, auto HighMember, size_t Offset>
struct NibbleField {
    template <typename Struct>
    static void write(uint8_t *out, size_t base, const Struct &source) {
        const uint8_t low = static_cast<uint8_t>(source.*LowMember);
        const uint8_t high = static_cast<uint8_t>(source.*HighMember);
        out[base + Offset] = static_cast<uint8_t>((high << 4) | low);
    }
};

template <typename Struct, typename... Fields>
inline void write_fields(uint8_t *out, size_t base, const Struct &source) {
    (Fields::write(out, base, source), ...);
}

} // namespace vsc::binary
