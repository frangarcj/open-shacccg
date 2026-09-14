#include "backend/machine_ir.hpp"

#include <cstdio>

namespace {
int fail(const char *message) {
    std::fprintf(stderr, "test_machine_ir: %s\n", message);
    return 1;
}

bool append_compare_kill(vsc::backend::MachineProgram &program,
                         vsc::backend::MachineOperand predicate,
                         uint8_t rhs_register) {
    using namespace vsc;
    using namespace backend;
    const auto lhs = program.physical<MachineType::U32>(usse::RegisterBank::PrimaryAttribute, 0);
    const auto rhs = program.physical<MachineType::U32>(usse::RegisterBank::SecondaryAttribute, rhs_register);
    return program.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal),
                                                predicate, lhs, rhs) &&
        program.emit<MachineOpcode::Kill>(0, {}, predicate);
}
} // namespace

int test_machine_ir() {
    using namespace vsc;
    using namespace backend;
    int failures = 0;

    {
        MachineProgram program;
        const auto predicate = program.make_predicate();
        if (!append_compare_kill(program, predicate, 8)) {
            failures += fail("could not construct compare/kill machine program");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program, result)) {
                failures += fail("compare/kill machine program did not compile");
            } else {
                if (result.words.size() != 2) failures += fail("compare/kill emitted wrong instruction count");
                if (result.words.size() == 2 && result.words[0] != 0x48880181b007c008ULL)
                    failures += fail("machine compare did not reproduce known VTST word");
                if (result.words.size() == 2 && result.words[1] != 0xf9300206f0000000ULL)
                    failures += fail("machine kill did not emit canonical P0 KILL");
                if (result.predicate_registers.size() != 1 || result.predicate_registers[0] != 0)
                    failures += fail("predicate allocator did not choose P0 for the simple interval");
            }
        }
    }

    // A predicate whose last read is the guard of an instruction may be
    // overwritten by that instruction's predicate destination. This matches
    // the real !p0 CMP -> p0 form and avoids artificial predicate pressure.
    {
        MachineProgram program;
        const auto first = program.make_predicate();
        const auto second = program.make_predicate();
        const auto lhs = program.physical<MachineType::U32>(usse::RegisterBank::PrimaryAttribute, 0);
        const auto rhs8 = program.physical<MachineType::U32>(usse::RegisterBank::SecondaryAttribute, 8);
        const auto rhs6 = program.physical<MachineType::U32>(usse::RegisterBank::SecondaryAttribute, 6);
        if (!program.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal),
                                                  first, lhs, rhs8) ||
            !program.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal),
                                                  second, lhs, rhs6,
                                                  MachineOperand::virtual_predicate(first.id(), true)))
            failures += fail("could not construct predicated compare chain");
        MachineCompileResult result;
        if (!compile_machine_program(program, result)) {
            failures += fail("predicated compare chain did not compile");
        } else {
            if (result.predicate_registers.size()!=2 || result.predicate_registers[0]!=0 || result.predicate_registers[1]!=0)
                failures += fail("dead guard predicate was not coalesced with destination");
            if (result.words.size()!=2 || result.words[1]!=0x4d880181b007c006ULL)
                failures += fail("predicated machine compare did not reproduce known word");
        }
    }

    // Sequential predicate lifetimes should reuse the same hardware predicate.
    {
        MachineProgram program;
        for (uint8_t i = 0; i < 6; ++i) {
            const auto predicate = program.make_predicate();
            if (!append_compare_kill(program, predicate, static_cast<uint8_t>(6 + i)))
                failures += fail("could not construct predicate reuse program");
        }
        MachineCompileResult result;
        if (!compile_machine_program(program, result)) {
            failures += fail("predicate reuse program did not compile");
        } else {
            for (uint8_t physical : result.predicate_registers)
                if (physical != 0) failures += fail("non-overlapping predicates were not recycled");
        }
    }

    // KILL can encode only the short P0/P1 predicate set, so three overlapping
    // kill predicates must fail instead of silently choosing an invalid register.
    {
        MachineProgram program;
        MachineOperand predicates[3] = {
            program.make_predicate(), program.make_predicate(), program.make_predicate()
        };
        const auto lhs = program.physical<MachineType::U32>(usse::RegisterBank::PrimaryAttribute, 0);
        for (uint8_t i = 0; i < 3; ++i) {
            const auto rhs = program.physical<MachineType::U32>(usse::RegisterBank::SecondaryAttribute,
                                                                 static_cast<uint8_t>(6 + i));
            if (!program.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal),
                                                       predicates[i], lhs, rhs))
                failures += fail("could not construct overlapping predicate definition");
        }
        for (const auto predicate : predicates)
            if (!program.emit<MachineOpcode::Kill>(0, {}, predicate))
                failures += fail("could not construct overlapping predicate use");

        MachineCompileResult result;
        if (compile_machine_program(program, result))
            failures += fail("predicate pressure beyond KILL encoding limits was accepted");
    }

    return failures;
}
