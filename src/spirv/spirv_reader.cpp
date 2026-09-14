#include "core/internal.hpp"
#include <algorithm>
#include <cstring>

namespace vsc {
namespace {
constexpr uint32_t kSpirvMagic = 0x07230203u;
constexpr uint16_t kOpEntryPoint = 15;
constexpr uint16_t kOpFunction = 54;
constexpr uint16_t kOpVariable = 59;

std::string read_literal_string(const uint32_t *words, size_t count) {
    const char *bytes = reinterpret_cast<const char *>(words);
    const size_t max_bytes = count * sizeof(uint32_t);
    size_t n = 0;
    while (n < max_bytes && bytes[n] != '\0') ++n;
    if (n == max_bytes) return {};
    return std::string(bytes, n);
}
}

bool parse_spirv(const uint32_t *words, size_t word_count,
                 SpirvSummary &summary, Diagnostic &error) {
    summary = {};
    error = {};
    if (!words || word_count < 5) {
        error = {VSC_DIAG_ERROR, 0x2101, 0, 0, "SPIR-V module is shorter than its 5-word header"};
        return false;
    }
    if (words[0] != kSpirvMagic) {
        error = {VSC_DIAG_ERROR, 0x2102, 0, 0, "invalid SPIR-V magic"};
        return false;
    }
    if (words[4] != 0) {
        error = {VSC_DIAG_ERROR, 0x2103, 0, 0, "SPIR-V reserved header word must be zero"};
        return false;
    }

    summary.version = words[1];
    summary.generator = words[2];
    summary.bound = words[3];
    if (summary.bound == 0) {
        error = {VSC_DIAG_ERROR, 0x2104, 0, 0, "SPIR-V id bound must be non-zero"};
        return false;
    }

    size_t cursor = 5;
    while (cursor < word_count) {
        const uint32_t first = words[cursor];
        const uint16_t wc = static_cast<uint16_t>(first >> 16);
        const uint16_t op = static_cast<uint16_t>(first & 0xffffu);
        if (wc == 0) {
            error = {VSC_DIAG_ERROR, 0x2105, 0, 0, "SPIR-V instruction has zero word count"};
            return false;
        }
        if (cursor + wc > word_count) {
            error = {VSC_DIAG_ERROR, 0x2106, 0, 0, "SPIR-V instruction extends past end of module"};
            return false;
        }
        ++summary.instruction_count;

        if (op == kOpEntryPoint) {
            if (wc < 4) {
                error = {VSC_DIAG_ERROR, 0x2107, 0, 0, "malformed OpEntryPoint"};
                return false;
            }
            SpirvEntryPoint ep;
            ep.execution_model = words[cursor + 1];
            ep.id = words[cursor + 2];
            ep.name = read_literal_string(words + cursor + 3, wc - 3);
            if (ep.name.empty()) {
                error = {VSC_DIAG_ERROR, 0x2108, 0, 0, "OpEntryPoint has an invalid or empty name"};
                return false;
            }
            summary.entry_points.push_back(std::move(ep));
        } else if (op == kOpVariable) {
            ++summary.variable_count;
        } else if (op == kOpFunction) {
            ++summary.function_count;
        }
        cursor += wc;
    }

    if (cursor != word_count) {
        error = {VSC_DIAG_ERROR, 0x2109, 0, 0, "SPIR-V parser did not terminate on module boundary"};
        return false;
    }
    return true;
}

} // namespace vsc
