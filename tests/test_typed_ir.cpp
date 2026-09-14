#include "backend/typed_ir.hpp"
#include "spirv/spirv_cross_adapter.hpp"
#include "spirv/spirv_pipeline.hpp"
#include "usse/usse.hpp"

#include <cstdio>
#include <cstring>
#include <vector>

namespace {
int fail(const char *message) {
    std::fprintf(stderr, "test_typed_ir: %s\n", message);
    return 1;
}

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
constexpr uint32_t spv_inst(uint16_t wc, uint16_t op) { return (uint32_t(wc) << 16) | op; }
void spv_append(std::vector<uint32_t> &m, uint16_t op, std::initializer_list<uint32_t> args) {
    m.push_back(spv_inst(static_cast<uint16_t>(args.size() + 1), op));
    m.insert(m.end(), args.begin(), args.end());
}
void spv_string(std::vector<uint32_t> &m, uint16_t op, std::initializer_list<uint32_t> prefix,
                const char *text, std::initializer_list<uint32_t> suffix = {}) {
    const size_t bytes = std::strlen(text) + 1;
    std::vector<uint32_t> words((bytes + 3) / 4, 0);
    std::memcpy(words.data(), text, bytes);
    m.push_back(spv_inst(static_cast<uint16_t>(1 + prefix.size() + words.size() + suffix.size()), op));
    m.insert(m.end(), prefix.begin(), prefix.end());
    m.insert(m.end(), words.begin(), words.end());
    m.insert(m.end(), suffix.begin(), suffix.end());
}

std::vector<uint32_t> make_u32_discard_spirv() {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,32u,0u};
    spv_append(m,17,{1});             // Capability Shader
    spv_append(m,14,{0,1});           // MemoryModel Logical GLSL450
    spv_string(m,15,{4,20},"main",{10,11});
    spv_append(m,16,{20,7});          // OriginUpperLeft
    spv_append(m,71,{10,30,0});       // Location 0
    spv_append(m,71,{11,30,1});       // Location 1
    spv_append(m,19,{1});             // void
    spv_append(m,20,{2});             // bool
    spv_append(m,21,{3,32,0});        // u32
    spv_append(m,32,{4,1,3});         // ptr Input u32
    spv_append(m,33,{5,1});           // void()
    spv_append(m,59,{4,10,1});
    spv_append(m,59,{4,11,1});
    spv_append(m,54,{1,20,0,5});
    spv_append(m,248,{21});
    spv_append(m,61,{3,22,10});
    spv_append(m,61,{3,23,11});
    spv_append(m,197,{3,24,22,23});   // BitwiseOr
    spv_append(m,170,{2,25,24,23});   // IEqual
    spv_append(m,247,{27,0});         // SelectionMerge
    spv_append(m,250,{25,26,27});     // BranchConditional
    spv_append(m,248,{26});
    spv_append(m,252,{});              // Kill
    spv_append(m,248,{27});
    spv_append(m,253,{});
    spv_append(m,56,{});
    return m;
}
#endif
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

#if defined(OPENSHACCG_ENABLE_SPIRV_CROSS)
    {
        const auto source = make_u32_discard_spirv();
        PreparedSpirv prepared;
        Diagnostic diagnostic;
        if (!prepare_spirv(VSC_STAGE_FRAGMENT, "main", source.data(), source.size(), prepared, diagnostic)) {
            failures += fail("SPIR-V preparation rejected U32 discard fixture");
        } else {
            TypedProgram program;
            std::string error;
            if (!spirv_cross_to_typed_fragment(prepared.words, "main", program, error)) {
                failures += fail("SPIRV-Cross did not lower U32 discard fixture to Typed IR");
            } else {
                MachineCompileResult result;
                if (!compile_typed_program(program, result)) {
                    failures += fail("SPIRV-Cross Typed IR did not compile to Machine IR");
                } else if (result.words.size() != 3) {
                    failures += fail("SPIRV-Cross U32 discard path emitted wrong instruction count");
                } else {
                    usse::VbwSemantic bitwise{};
                    usse::VtstSemantic compare{};
                    if (!usse::decode_vbw_semantic(result.words[0], &bitwise) || bitwise.op != usse::BitwiseOp::Or ||
                        bitwise.dst.bank != usse::RegisterBank::Temp || bitwise.src1.bank != usse::RegisterBank::PrimaryAttribute)
                        failures += fail("SPIRV-Cross Typed IR emitted unexpected VBW");
                    if (!usse::decode_vtst_semantic(result.words[1], &compare) ||
                        compare.lhs.bank != usse::RegisterBank::Temp || compare.rhs.bank != usse::RegisterBank::PrimaryAttribute)
                        failures += fail("SPIRV-Cross Typed IR emitted unexpected VTST");
                }
            }
        }
    }
#endif

    return failures;
}
