#pragma once

#include "core/internal.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vsc {

struct PreparedSpirv {
    std::vector<uint32_t> words;
    bool optimized = false;
    bool cross_validated = false;
};

bool prepare_spirv(VscStage stage, const char *entrypoint,
                   const uint32_t *words, size_t word_count,
                   PreparedSpirv &out, Diagnostic &error);

} // namespace vsc
