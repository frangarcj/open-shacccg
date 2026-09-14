#pragma once

#include <cstdint>
#include <type_traits>

namespace vsc::usse::detail {

constexpr uint64_t bit_mask(unsigned width) {
    return width == 64 ? ~uint64_t{0} : ((uint64_t{1} << width) - 1);
}

template <typename T>
constexpr T extract(uint64_t word, unsigned offset, unsigned width) {
    return static_cast<T>((word >> offset) & bit_mask(width));
}

template <auto Member, unsigned Offset, unsigned Width>
struct BitField {
    static_assert(Width > 0 && Width < 64, "invalid encoded field width");
    static_assert(Offset + Width <= 64, "encoded field exceeds instruction width");

    template <typename Struct>
    static void decode(uint64_t word, Struct &out) {
        using Value = std::remove_reference_t<decltype(out.*Member)>;
        out.*Member = static_cast<Value>(extract<uint64_t>(word, Offset, Width));
    }

    template <typename Struct>
    static bool encode(const Struct &in, uint64_t &word) {
        const uint64_t value = static_cast<uint64_t>(in.*Member);
        if (value > bit_mask(Width)) return false;
        word |= value << Offset;
        return true;
    }
};

template <uint64_t Mask, uint64_t Expected, typename... Fields>
struct Encoding {
    static_assert((Expected & ~Mask) == 0, "expected bits must be covered by mask");

    static constexpr uint64_t mask = Mask;
    static constexpr uint64_t expected = Expected;

    static constexpr bool matches(uint64_t word) {
        return (word & mask) == expected;
    }

    template <typename Struct>
    static bool decode(uint64_t word, Struct *out) {
        if (!out || !matches(word)) return false;
        (Fields::decode(word, *out), ...);
        return true;
    }

    template <typename Struct>
    static bool encode(const Struct &in, uint64_t *word) {
        if (!word) return false;
        uint64_t encoded = expected;
        if (!(Fields::encode(in, encoded) && ...)) return false;
        *word = encoded;
        return true;
    }
};

} // namespace vsc::usse::detail
