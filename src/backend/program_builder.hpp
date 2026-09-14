#pragma once

#include "usse/usse.hpp"

#include <cstdint>
#include <vector>

namespace vsc::backend {

// Small semantic USSE program assembler used by the backend lowering path.
// It intentionally exposes only instruction families with evidence-backed
// encoders; unsupported operations fail instead of guessing machine words.
class ProgramBuilder {
public:
    bool phase();
    bool nop();
    bool emit();

    template <typename Semantic>
    bool instruction(const Semantic &instruction) {
        uint64_t word = 0;
        const bool ok = usse::encode_semantic(instruction, &word);
        return append(ok, word);
    }

    const std::vector<uint64_t> &words() const { return words_; }
    void clear() { words_.clear(); }

private:
    bool append_control(usse::Opcode opcode);
    bool append(bool ok, uint64_t word);

    std::vector<uint64_t> words_;
};

} // namespace vsc::backend
