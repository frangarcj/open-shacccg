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

bool ProgramBuilder::vmov(const usse::VmovSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_vmov_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::vpck(const usse::VpckSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_vpck_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::v32nmad(const usse::V32NmadSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_v32nmad_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::vmad(const usse::VmadSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_vmad_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::vtst(const usse::VtstSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_vtst_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::vbw(const usse::VbwSemantic &instruction) {
    uint64_t word = 0;
    const bool ok = usse::encode_vbw_semantic(instruction, &word);
    return append(ok, word);
}

bool ProgramBuilder::kill(usse::Predicate predicate) {
    usse::KillSemantic instruction{};
    instruction.predicate = predicate;
    uint64_t word = 0;
    const bool ok = usse::encode_kill_semantic(instruction, &word);
    return append(ok, word);
}

} // namespace vsc::backend
