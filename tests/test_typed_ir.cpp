#include "backend/typed_ir.hpp"
#include "usse/usse.hpp"

#include <cstdio>

namespace {
int fail(const char *message) {
    std::fprintf(stderr, "test_typed_ir: %s\n", message);
    return 1;
}
} // namespace

int test_typed_ir() {
    using namespace vsc;
    using namespace backend;
    int failures = 0;

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::U32>(0);
        const auto rhs = program.uniform<TypedType::U32>(8);
        const auto pred = program.make_predicate();
        if (!program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal), pred, lhs, rhs) ||
            !program.emit<TypedOpcode::Discard>(0, {}, pred)) {
            failures += fail("could not construct typed compare/discard program");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program, result)) {
                failures += fail("typed compare/discard program did not compile");
            } else {
                if (result.words.size() != 2 || result.words[0] != 0x48880181b007c008ULL)
                    failures += fail("typed compare did not reproduce known VTST word");
                if (result.words.size() != 2 || result.words[1] != 0xf9300206f0000000ULL)
                    failures += fail("typed discard did not reproduce canonical KILL word");
            }
        }
    }

    {
        TypedProgram program;
        const auto source = program.uniform<TypedType::U32>(12);
        const auto zero = program.literal_u32(0);
        const auto masked = program.make_value<TypedType::U32>();
        const auto rhs = program.uniform<TypedType::U32>(6);
        const auto pred = program.make_predicate();
        if (!program.emit<TypedOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or), masked, source, zero) ||
            !program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal), pred, masked, rhs) ||
            !program.emit<TypedOpcode::Discard>(0, {}, pred)) {
            failures += fail("could not construct typed bitwise/compare/discard program");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program, result)) {
                failures += fail("typed bitwise/compare/discard program did not compile");
            } else {
                if (result.words.size() != 3) {
                    failures += fail("typed bitwise pipeline emitted wrong instruction count");
                } else {
                    usse::VbwSemantic bitwise{};
                    if (!usse::decode_vbw_semantic(result.words[0], &bitwise) ||
                        bitwise.op != usse::BitwiseOp::Or || bitwise.dst.bank != usse::RegisterBank::Temp ||
                        bitwise.dst.num != 0 || bitwise.src1.bank != usse::RegisterBank::SecondaryAttribute ||
                        bitwise.src1.num != 12 || !bitwise.src2_is_immediate || bitwise.immediate != 0)
                        failures += fail("typed bitwise lowering produced unexpected VBW operands");
                    usse::VtstSemantic compare{};
                    if (!usse::decode_vtst_semantic(result.words[1], &compare) ||
                        compare.lhs.bank != usse::RegisterBank::Temp || compare.lhs.num != 0 ||
                        compare.rhs.bank != usse::RegisterBank::SecondaryAttribute || compare.rhs.num != 6)
                        failures += fail("typed compare did not consume allocated TEMP value");
                }
                if (result.value_registers.size() != 1 || result.value_registers[0].bank != usse::RegisterBank::Temp ||
                    result.value_registers[0].num != 0)
                    failures += fail("typed U32 temporary was not allocated to TEMP0");
            }
        }
    }

    {
        TypedProgram program;
        for (uint16_t i = 0; i < 3; ++i) {
            const auto source = program.uniform<TypedType::U32>(static_cast<uint16_t>(12 + i));
            const auto zero = program.literal_u32(0);
            const auto value = program.make_value<TypedType::U32>();
            const auto rhs = program.uniform<TypedType::U32>(static_cast<uint16_t>(6 + i));
            const auto pred = program.make_predicate();
            if (!program.emit<TypedOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or), value, source, zero) ||
                !program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal), pred, value, rhs) ||
                !program.emit<TypedOpcode::Discard>(0, {}, pred))
                failures += fail("could not construct typed TEMP reuse chain");
        }
        MachineCompileResult result;
        if (!compile_typed_program(program, result)) {
            failures += fail("typed TEMP reuse chain did not compile");
        } else {
            for (const auto reg : result.value_registers)
                if (reg.bank != usse::RegisterBank::Temp || reg.num != 0)
                    failures += fail("non-overlapping typed U32 values did not recycle TEMP0");
        }
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32>(0);
        const auto rhs = program.uniform<TypedType::F32>(0);
        const auto dst = program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or), dst, lhs, rhs);
        MachineCompileResult result;
        if (compile_typed_program(program, result))
            failures += fail("typed bitwise accepted unsupported F32 operands");
    }

    return failures;
}
