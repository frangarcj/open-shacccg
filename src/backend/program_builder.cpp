#include "backend/program_builder.hpp"

namespace vsc::backend {

bool ProgramBuilder::append(bool ok, uint64_t word) {
    if (!ok) return false;
    words_.push_back(word);
    return true;
}

bool ProgramBuilder::append_control(usse::Opcode opcode) {
    usse::Instruction instruction{};
    instruction.opcode = opcode;
    uint64_t word = 0;
    const bool ok = usse::encode(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::phase() { return append_control(usse::Opcode::Phase); }
bool ProgramBuilder::nop() { return append_control(usse::Opcode::Nop); }
bool ProgramBuilder::emit() { return append_control(usse::Opcode::Emit); }

} // namespace vsc::backend
