#include "backend/typed_ir.hpp"
#include "backend/shader_profiles.hpp"
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

std::vector<uint32_t> make_u32_if_else_spirv() {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,48u,0u};
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
    spv_append(m,170,{2,24,22,23});   // IEqual
    spv_append(m,247,{30,0});         // SelectionMerge
    spv_append(m,250,{24,26,27});     // BranchConditional
    spv_append(m,248,{26});
    spv_append(m,197,{3,28,22,23});   // BitwiseOr
    spv_append(m,249,{30});            // Branch merge
    spv_append(m,248,{27});
    spv_append(m,198,{3,29,22,23});   // BitwiseXor
    spv_append(m,249,{30});            // Branch merge
    spv_append(m,248,{30});
    spv_append(m,253,{});
    spv_append(m,56,{});
    return m;
}

std::vector<uint32_t> make_f32_if_else_spirv() {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,40u,0u};
    spv_append(m,17,{1});             // Capability Shader
    spv_append(m,14,{0,1});           // MemoryModel Logical GLSL450
    spv_string(m,15,{4,20},"main",{10,11});
    spv_append(m,16,{20,7});          // OriginUpperLeft
    spv_append(m,71,{10,30,0});       // Location 0 -> PA0
    spv_append(m,71,{11,30,2});       // Location 2 -> PA2, matching oracle anchors
    spv_append(m,19,{1});             // void
    spv_append(m,20,{2});             // bool
    spv_append(m,22,{3,32});          // f32
    spv_append(m,32,{4,1,3});         // ptr Input f32
    spv_append(m,33,{5,1});           // void()
    spv_append(m,59,{4,10,1});
    spv_append(m,59,{4,11,1});
    spv_append(m,54,{1,20,0,5});
    spv_append(m,248,{21});
    spv_append(m,61,{3,22,10});
    spv_append(m,61,{3,23,11});
    spv_append(m,186,{2,24,22,23});   // FOrdGreaterThan
    spv_append(m,247,{30,0});         // SelectionMerge
    spv_append(m,250,{24,26,27});     // BranchConditional
    spv_append(m,248,{26});
    spv_append(m,249,{30});            // Branch merge
    spv_append(m,248,{27});
    spv_append(m,249,{30});            // Branch merge
    spv_append(m,248,{30});
    spv_append(m,253,{});
    spv_append(m,56,{});
    return m;
}

std::vector<uint32_t> make_float_extinst_spirv() {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,40u,0u};
    spv_append(m,17,{1});                         // Capability Shader
    spv_append(m,14,{0,1});                       // MemoryModel Logical GLSL450
    spv_string(m,11,{7},"GLSL.std.450");         // ExtInstImport
    spv_string(m,15,{4,20},"main",{10,11});
    spv_append(m,16,{20,7});                      // OriginUpperLeft
    spv_string(m,5,{10},"vColor");
    spv_string(m,5,{11},"fragColor");
    spv_append(m,71,{10,30,0});                   // Location 0
    spv_append(m,71,{11,30,0});
    spv_append(m,19,{1});                         // void
    spv_append(m,22,{2,32});                      // f32
    spv_append(m,23,{3,2,4});                     // float4
    spv_append(m,32,{4,1,3});                     // ptr Input float4
    spv_append(m,32,{5,3,3});                     // ptr Output float4
    spv_append(m,33,{6,1});                       // void()
    spv_append(m,59,{4,10,1});
    spv_append(m,59,{5,11,3});
    spv_append(m,54,{1,20,0,6});
    spv_append(m,248,{21});
    spv_append(m,61,{3,22,10});
    spv_append(m,79,{3,23,22,22,0,0,0,0});      // VectorShuffle xxxx
    spv_append(m,12,{3,24,7,4,23});              // GLSL FAbs
    spv_append(m,12,{3,25,7,37,24,22});          // GLSL FMin
    spv_append(m,12,{3,26,7,40,25,22});          // GLSL FMax
    spv_append(m,62,{11,26});
    spv_append(m,253,{});
    spv_append(m,56,{});
    return m;
}

std::vector<uint32_t> make_float_convert_spirv() {
    std::vector<uint32_t> m={0x07230203u,0x00010000u,0u,32u,0u};
    spv_append(m,17,{1});                         // Capability Shader
    spv_append(m,17,{9});                         // Capability Float16
    spv_append(m,14,{0,1});                       // MemoryModel Logical GLSL450
    spv_string(m,15,{4,20},"main",{10});
    spv_append(m,16,{20,7});                      // OriginUpperLeft
    spv_append(m,71,{10,30,0});                   // Location 0
    spv_append(m,19,{1});                         // void
    spv_append(m,22,{2,32});                      // f32
    spv_append(m,23,{3,2,4});                     // float4
    spv_append(m,22,{4,16});                      // f16
    spv_append(m,23,{5,4,4});                     // half4
    spv_append(m,32,{6,1,3});                     // ptr Input float4
    spv_append(m,33,{7,1});                       // void()
    spv_append(m,59,{6,10,1});
    spv_append(m,54,{1,20,0,7});
    spv_append(m,248,{21});
    spv_append(m,61,{3,22,10});
    spv_append(m,115,{5,23,22});                  // FConvert float4 -> half4
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
        const auto predicate = program.make_predicate();
        const uint16_t true_label = program.make_label();
        const uint16_t merge_label = program.make_label();
        const auto copied = program.make_value<TypedType::U32>();
        const auto zero = program.literal_u32(0);
        if (!program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Equal), predicate, lhs, rhs) ||
            !program.branch(true_label,predicate) || !program.jump(merge_label) ||
            !program.bind_label(true_label) ||
            !program.emit<TypedOpcode::Bitwise>(static_cast<uint8_t>(usse::BitwiseOp::Or),copied,lhs,zero) ||
            !program.bind_label(merge_label)) {
            failures += fail("could not construct typed branch program");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result)) {
                failures += fail("typed branch program did not compile");
            } else if (result.words.size()!=4 || result.words[1]!=0xf900004000000002ULL ||
                       result.words[2]!=0xf800004000000002ULL) {
                failures += fail("typed branch program emitted unexpected BR words");
            }
        }
    }

    {
        TypedProgram program;
        const auto zero=program.literal_s32(0);
        const auto counter=program.make_value<TypedType::S32>();
        const auto limit=program.uniform<TypedType::S32>(0);
        const auto predicate=program.make_predicate();
        if (!program.emit<TypedOpcode::StateInit>(0,counter,zero) ||
            !program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Less),predicate,counter,limit) ||
            !program.emit<TypedOpcode::IntIncrement>(1,counter)) {
            failures += fail("could not construct Typed loop counter primitives");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result) || result.words.size()!=4 ||
                result.words[0]!=0x50810008e0000100ULL ||
                result.words[1]!=0x48a8068130078000ULL ||
                result.words[2]!=0xd08180042020c001ULL ||
                result.words[3]!=0xd09080040000c001ULL)
                failures += fail("Typed loop counter primitives did not reproduce oracle words");
        }
    }

    {
        TypedProgram program;
        const auto input=program.input<TypedType::F32x4>(0);
        const auto state=program.make_value<TypedType::F32x4>();
        if (!program.emit<TypedOpcode::StateInit>(0,state,input) ||
            !program.emit<TypedOpcode::StateUpdate>(0,state,input)) {
            failures += fail("could not construct Typed mutable float4 state");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result) || result.words.size()!=2)
                failures += fail("Typed mutable float4 state did not lower through Machine IR");
        }
    }

    {
        TypedProgram program;
        const auto lhs=program.input<TypedType::F32>(0);
        const auto rhs=program.input<TypedType::F32>(2);
        const auto predicate=program.make_predicate();
        if (!program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Greater),predicate,lhs,rhs)) {
            failures += fail("could not construct typed F32 compare");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result) || result.words.size()!=1 ||
                result.words[0]!=0x48088a81a0038002ULL)
            failures += fail("typed F32 compare did not reproduce oracle VTST word");
        }
    }

    {
        TypedProgram program;
        const auto lhs=program.uniform<TypedType::F32>(0);
        const auto half=program.literal_f32(0x3f000000u);
        const auto predicate=program.make_predicate();
        if (!program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Greater),predicate,lhs,half)) {
            failures += fail("could not construct typed uniform > 0.5 compare");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result) || result.words.size()!=1 ||
                result.words[0]!=0x48898a81d003800cULL)
                failures += fail("typed uniform > 0.5 compare did not reproduce oracle VTST word");
        }
    }

    {
        TypedProgram program;
        const auto lhs=program.input_component_f32(0,0);
        const auto rhs=program.input_component_f32(0,1);
        const auto dst=program.make_value<TypedType::F32>();
        if (!program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Add),dst,lhs,rhs)) {
            failures += fail("could not construct packed scalar F32 arithmetic");
        } else {
            MachineCompileResult result;
            usse::V32NmadSemantic add{};
            if (!compile_typed_program(program,result) || result.words.size()!=1 ||
                !usse::decode_v32nmad_semantic(result.words[0],&add) || add.op!=usse::VectorOp::Add ||
                add.dest_mask!=1 || add.src1.bank!=usse::RegisterBank::PrimaryAttribute || add.src1.num!=0 ||
                add.src2.bank!=usse::RegisterBank::PrimaryAttribute || add.src2.num!=0 ||
                add.src1_swizzle.c[0]!=usse::SwizzleChannel::X || add.src1_swizzle.c[3]!=usse::SwizzleChannel::X ||
                add.src2_swizzle.c[0]!=usse::SwizzleChannel::Y || add.src2_swizzle.c[3]!=usse::SwizzleChannel::Y)
                failures += fail("packed scalar F32 inputs did not lower to component-selected V32NMAD");
        }
    }

    {
        TypedProgram program;
        const uint16_t label=program.make_label();
        program.jump(label);
        MachineCompileResult result;
        if (compile_typed_program(program,result))
            failures += fail("typed control flow accepted an unbound label");
    }

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
        const auto lhs = program.input<TypedType::F32x4>(0);
        const auto rhs = program.uniform<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F32x4>();
        if (!program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Mul), dst, lhs, rhs)) {
            failures += fail("could not construct typed float4 multiply");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program, result)) {
                failures += fail("typed float4 multiply did not lower to Machine IR");
            } else {
                usse::V32NmadSemantic mul{};
                if (result.words.size()!=1 || !usse::decode_v32nmad_semantic(result.words[0], &mul) ||
                    mul.op!=usse::VectorOp::Mul || mul.dst.bank!=usse::RegisterBank::Temp || mul.dst.num!=4 ||
                    mul.src1.bank!=usse::RegisterBank::PrimaryAttribute ||
                    mul.src2.bank!=usse::RegisterBank::SecondaryAttribute)
                    failures += fail("typed float4 multiply emitted unexpected V32NMAD");
            }
        }
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32x4>(0);
        const auto rhs = program.uniform<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F32x4>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Sub), dst, lhs, rhs);
        MachineCompileResult result;
        usse::V32NmadSemantic sub{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0], &sub) || sub.op!=usse::VectorOp::Add ||
            !sub.src1_negative || sub.src1.bank!=usse::RegisterBank::SecondaryAttribute ||
            sub.src2.bank!=usse::RegisterBank::PrimaryAttribute)
            failures += fail("typed float4 subtraction did not use validated negated-add form");
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32x4>(0);
        const auto rhs = program.uniform<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Dot), dst, lhs, rhs);
        MachineCompileResult result;
        usse::V32NmadSemantic dot{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0], &dot) || dot.op!=usse::VectorOp::Dot || dot.dest_mask!=1)
            failures += fail("typed float4 dot did not lower to scalar V32NMAD dot");
    }

    {
        TypedProgram program;
        const auto source = program.input<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F32x4>();
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Neg), dst, source);
        MachineCompileResult result;
        usse::V32NmadSemantic neg{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0], &neg) || neg.op!=usse::VectorOp::Add ||
            !neg.src1_negative || neg.src1.bank!=usse::RegisterBank::PrimaryAttribute ||
            neg.src2.bank!=usse::RegisterBank::Immediate)
            failures += fail("typed float4 negate did not lower to validated V32NMAD form");
    }

    {
        TypedProgram program;
        const auto source=program.input<TypedType::F32x4>(0);
        const auto dst=program.make_value<TypedType::F32x4>();
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Saturate),dst,source);
        MachineCompileResult result;
        if (!compile_typed_program(program,result) || result.words.size()!=2) {
            failures += fail("typed saturate did not lower to two validated vector operations");
        } else {
            usse::V32NmadSemantic low{},high{};
            if (!usse::decode_v32nmad_semantic(result.words[0],&low) || low.op!=usse::VectorOp::Max ||
                low.src2.bank!=usse::RegisterBank::Immediate)
                failures += fail("typed saturate lower clamp is not V32NMAD MAX(x,0)");
            if (!usse::decode_v32nmad_semantic(result.words[1],&high) || high.op!=usse::VectorOp::Min ||
                high.src2.bank!=usse::RegisterBank::Special ||
                high.src2_swizzle.c[0]!=usse::SwizzleChannel::Y || high.src2_swizzle.c[3]!=usse::SwizzleChannel::Y)
                failures += fail("typed saturate upper clamp is not V32NMAD MIN(x,SPECIAL1.yyyy)");
        }
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32x4>(0);
        const auto rhs = program.uniform<TypedType::F32x4>(0);
        const auto min_value = program.make_value<TypedType::F32x4>();
        const auto max_value = program.make_value<TypedType::F32x4>();
        const auto abs_value = program.make_value<TypedType::F32x4>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Min), min_value, lhs, rhs);
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Max), max_value, min_value, rhs);
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Abs), abs_value, max_value);
        MachineCompileResult result;
        if (!compile_typed_program(program, result) || result.words.size()!=3) {
            failures += fail("typed min/max/abs chain did not compile");
        } else {
            usse::V32NmadSemantic min_op{}, max_op{}, abs_op{};
            if (!usse::decode_v32nmad_semantic(result.words[0], &min_op) || min_op.op!=usse::VectorOp::Min)
                failures += fail("typed FMin did not lower to V32NMAD MIN");
            if (!usse::decode_v32nmad_semantic(result.words[1], &max_op) || max_op.op!=usse::VectorOp::Max)
                failures += fail("typed FMax did not lower to V32NMAD MAX");
            if (!usse::decode_v32nmad_semantic(result.words[2], &abs_op) || abs_op.op!=usse::VectorOp::Add ||
                !abs_op.src1_absolute || abs_op.src2.bank!=usse::RegisterBank::Immediate)
                failures += fail("typed FAbs did not lower to validated absolute-add form");
        }
    }

    {
        TypedProgram program;
        const auto source = program.input<TypedType::F32>(0);
        const auto dst = program.make_value<TypedType::F32x4>();
        program.emit<TypedOpcode::FloatSplat>(0, dst, source);
        MachineCompileResult result;
        usse::VmovSemantic splat{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_vmov_semantic(result.words[0], &splat) ||
            splat.data_type!=usse::DataType::F32 || splat.swizzle!=0 || splat.dest_mask!=0xF ||
            splat.src.bank!=usse::RegisterBank::PrimaryAttribute || splat.dst.bank!=usse::RegisterBank::Temp)
            failures += fail("typed scalar splat did not lower to validated VMOV swizzle-0 form");
    }

    {
        TypedProgram program;
        const auto source=program.input<TypedType::F32x3>(0);
        const auto y=program.make_value<TypedType::F32>();
        const auto neg=program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::FloatExtract>(1,y,source);
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Neg),neg,y);
        MachineCompileResult result;
        usse::V32NmadSemantic op{};
        if (!compile_typed_program(program,result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0],&op) || !op.src1_negative ||
            op.src1.bank!=usse::RegisterBank::PrimaryAttribute ||
            op.src1_swizzle.c[0]!=usse::SwizzleChannel::Y ||
            op.src1_swizzle.c[1]!=usse::SwizzleChannel::Y ||
            op.src1_swizzle.c[2]!=usse::SwizzleChannel::Y ||
            op.src1_swizzle.c[3]!=usse::SwizzleChannel::Y)
            failures += fail("typed float component extraction did not alias the physical source lane");
    }

    {
        TypedProgram program;
        const auto source=program.input<TypedType::F32>(0);
        const auto upper=program.uniform<TypedType::F32>(0);
        const auto zero=program.literal_f32(0);
        const auto low=program.make_value<TypedType::F32>();
        const auto clamped=program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Max),low,source,zero);
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Min),clamped,low,upper);
        MachineCompileResult result;
        usse::V32NmadSemantic max_op{},min_op{};
        if (!compile_typed_program(program,result) || result.words.size()!=2 ||
            !usse::decode_v32nmad_semantic(result.words[0],&max_op) || max_op.op!=usse::VectorOp::Max ||
            max_op.src2.bank!=usse::RegisterBank::Immediate ||
            !usse::decode_v32nmad_semantic(result.words[1],&min_op) || min_op.op!=usse::VectorOp::Min ||
            min_op.src2.bank!=usse::RegisterBank::SecondaryAttribute)
            failures += fail("typed scalar dynamic clamp did not lower to MAX(x,0)/MIN(x,upper)");
    }

    {
        TypedProgram program;
        const auto numerator=program.input<TypedType::F32>(0);
        const auto denominator=program.uniform<TypedType::F32>(0);
        const auto quotient=program.make_value<TypedType::F32>();
        const auto logged=program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Div),quotient,numerator,denominator);
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Log2),logged,quotient);
        MachineCompileResult result;
        usse::VcompF32Semantic reciprocal{},log2{};
        usse::V32NmadSemantic multiply{};
        if (!compile_typed_program(program,result) || result.words.size()!=3 ||
            !usse::decode_vcomp_f32_semantic(result.words[0],&reciprocal) ||
            reciprocal.op!=usse::ComplexOp::Reciprocal || reciprocal.src.bank!=usse::RegisterBank::SecondaryAttribute ||
            !usse::decode_v32nmad_semantic(result.words[1],&multiply) || multiply.op!=usse::VectorOp::Mul ||
            !usse::decode_vcomp_f32_semantic(result.words[2],&log2) || log2.op!=usse::ComplexOp::Log2 ||
            log2.src.bank!=usse::RegisterBank::Temp)
            failures += fail("typed scalar division/Log2 did not lower through VCOMP/V32NMAD");
    }

    {
        TypedProgram program;
        const auto source=program.input<TypedType::F32>(0);
        const auto floored=program.make_value<TypedType::F32>();
        program.emit<TypedOpcode::FloatUnary>(static_cast<uint8_t>(TypedFloatUnaryOp::Floor),floored,source);
        MachineCompileResult result;
        usse::V32NmadSemantic frac{},sub{};
        if (!compile_typed_program(program,result) || result.words.size()!=2 ||
            !usse::decode_v32nmad_semantic(result.words[0],&frac) || frac.op!=usse::VectorOp::Frac ||
            !usse::decode_v32nmad_semantic(result.words[1],&sub) || sub.op!=usse::VectorOp::Add || !sub.src1_negative)
            failures += fail("typed scalar Floor did not lower as x-frac(x)");
    }

    {
        TypedProgram program;
        const auto lhs=program.input<TypedType::F32>(0);
        const auto rhs=program.input<TypedType::F32>(2);
        const auto when_true=program.input<TypedType::F32>(4);
        const auto when_false=program.input<TypedType::F32>(6);
        const auto predicate=program.make_predicate();
        program.emit<TypedOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Greater),predicate,lhs,rhs);
        const auto selected=program.select_f32(predicate,when_true,when_false);
        MachineCompileResult result;
        usse::VmovSemantic initial{},update{};
        if (selected.kind()==TypedValueKind::None || !compile_typed_program(program,result) || result.words.size()!=3 ||
            !usse::decode_vmov_semantic(result.words[1],&initial) || initial.predicate!=usse::Predicate::Always ||
            !usse::decode_vmov_semantic(result.words[2],&update) || update.predicate!=usse::Predicate::P0)
            failures += fail("typed scalar FloatSelect did not lower to initialize + predicated update");
    }

    {
        TypedProgram program;
        const auto source=program.input<TypedType::F32x4>(0);
        const auto lhs=program.input<TypedType::F32x4>(1);
        std::array<TypedValue,4> lanes{};
        bool built=true;
        for (uint8_t lane=0;lane<4;++lane) {
            lanes[lane]=program.make_value<TypedType::F32>();
            built = built && lanes[lane].kind()!=TypedValueKind::None &&
                program.emit<TypedOpcode::FloatExtract>(lane,lanes[lane],source);
        }
        const auto composite=built ? program.compose_f32x4(lanes) : TypedValue{};
        const auto dot=program.make_value<TypedType::F32>();
        built = built && composite.kind()!=TypedValueKind::None && dot.kind()!=TypedValueKind::None &&
            program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Dot),dot,lhs,composite);
        MachineCompileResult result;
        usse::V32NmadSemantic dot_op{};
        if (!built || !compile_typed_program(program,result) || result.words.size()!=2 ||
            usse::classify_major(result.words[0])!=usse::MajorClass::Vmov ||
            !usse::decode_v32nmad_semantic(result.words[1],&dot_op) || dot_op.op!=usse::VectorOp::Dot)
            failures += fail("typed intermediate float4 compose did not materialize for dot consumption");
    }

    {
        // Execute the emitted compose moves on bit patterns, not just instruction
        // counts. Equal register numbers in different banks must not be merged.
        const uint8_t patterns[][4]={
            {0,1,2,3},{0,1,2,7},{0,5,2,7},{2,2,2,2},
            {3,2,1,0},{1,1,6,6},{0,1,1,3},{0,4,0,4},
        };
        const uint32_t bits[2][4]={
            {0x80000000u,0x7fc12345u,0x3f800000u,0x7f800000u},
            {0x00000000u,0xff800000u,0xbf800000u,0x00000001u},
        };
        for (unsigned storage=0;storage<3;++storage) for (unsigned width:{3u,4u}) {
            for (const auto &pattern:patterns) {
                TypedProgram program;
                const auto first=storage==1 ? program.uniform<TypedType::F32x4>(8) :
                                              program.input<TypedType::F32x4>(2);
                const auto second=storage==0 ? program.input<TypedType::F32x4>(5) :
                                               program.uniform<TypedType::F32x4>(storage==1 ? 20 : 8);
                const usse::RegisterRef registers[]={
                    {storage==1 ? usse::RegisterBank::SecondaryAttribute : usse::RegisterBank::PrimaryAttribute,4},
                    {storage==0 ? usse::RegisterBank::PrimaryAttribute : usse::RegisterBank::SecondaryAttribute,
                     static_cast<uint8_t>(storage==2 ? 4 : 10)},
                };
                std::array<TypedValue,4> lanes{};
                bool ok=true;
                for (uint8_t lane=0;lane<width;++lane) {
                    lanes[lane]=program.make_value<TypedType::F32>();
                    ok=ok && program.emit<TypedOpcode::FloatExtract>(pattern[lane]%4,lanes[lane],
                                                                     pattern[lane]<4 ? first : second);
                }
                const auto composite=width==4 ? program.compose_f32x4(lanes) :
                    program.compose_f32x3({lanes[0],lanes[1],lanes[2]});
                const auto lhs=program.input(width==4 ? TypedType::F32x4 : TypedType::F32x3,8);
                const auto product=program.make_value(width==4 ? TypedType::F32x4 : TypedType::F32x3);
                ok=ok && program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Mul),product,lhs,composite);
                MachineCompileResult result;
                ok=ok && compile_typed_program(program,result) && !result.words.empty();
                uint32_t actual[4]{};
                uint8_t written=0;
                usse::RegisterRef destination{};
                for (size_t index=0;ok && index+1<result.words.size();++index) {
                    usse::VmovSemantic move{};
                    ok=usse::decode_vmov_semantic(result.words[index],&move) &&
                       move.data_type==usse::DataType::F32 && move.swizzle<=4 && move.repeat_count==0 &&
                       move.predicate==usse::Predicate::Always && !(written & move.dest_mask);
                    if (!ok) break;
                    if (!index) destination=move.dst;
                    ok=move.dst.bank==destination.bank && move.dst.num==destination.num;
                    unsigned source=0;
                    while (source<2 && (registers[source].bank!=move.src.bank || registers[source].num!=move.src.num)) ++source;
                    if (source==2) { ok=false; break; }
                    for (uint8_t lane=0;lane<width;++lane)
                        if (move.dest_mask & (1u<<lane)) actual[lane]=bits[source][move.swizzle==4 ? lane : move.swizzle];
                    written |= move.dest_mask;
                }
                ok=ok && written==(1u<<width)-1u;
                for (uint8_t lane=0;lane<width;++lane)
                    ok=ok && actual[lane]==bits[pattern[lane]/4][pattern[lane]%4];
                if (pattern[0]==0 && pattern[1]==1 && pattern[2]==2)
                    ok=ok && result.words.size()==(width==3 || pattern[3]==3 ? 2u : 3u);
                if (!ok) {
                    std::fprintf(stderr,"compose storage=%u width=%u pattern=%u%u%u%u words=%zu mask=%u error=%s\n",
                        storage,width,pattern[0],pattern[1],pattern[2],pattern[3],result.words.size(),written,result.error.c_str());
                    failures += fail("generic float compose changed lane bits or failed to coalesce compatible writes");
                }
            }
        }
    }

    {
        TypedProgram program;
        const auto scalar=program.input<TypedType::F32>(0);
        const auto vector=program.input<TypedType::F32x4>(2);
        const auto computed=program.make_value<TypedType::F32>();
        bool ok=program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Mul),computed,scalar,scalar);
        const auto composite=program.compose_f32x4({computed,computed,computed,computed});
        const auto product=program.make_value<TypedType::F32x4>();
        ok=ok && program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Mul),product,vector,composite);
        MachineCompileResult result;
        usse::V32NmadSemantic calculation{};
        usse::VmovSemantic move{};
        if (!ok || !compile_typed_program(program,result) || result.words.size()!=3 ||
            !usse::decode_v32nmad_semantic(result.words[0],&calculation) ||
            !usse::decode_vmov_semantic(result.words[1],&move) || move.dest_mask!=0xf || move.swizzle!=0 ||
            move.src.bank!=calculation.dst.bank || move.src.num!=calculation.dst.num)
            failures += fail("generic compose did not broadcast the computed scalar from its allocated register");
    }

    {
        TypedProgram program;
        const auto source = program.input<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F16x4>();
        program.emit<TypedOpcode::FloatConvert>(
            static_cast<uint8_t>(TypedFloatConvertOp::F32x4ToF16x4), dst, source);
        MachineCompileResult result;
        usse::VpckSemantic pack{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_vpck_semantic(result.words[0], &pack) ||
            pack.src_format!=usse::PackFormat::F32 || pack.dst_format!=usse::PackFormat::F16 ||
            pack.src1.bank!=usse::RegisterBank::PrimaryAttribute || pack.src1.num!=0 || pack.src2.num!=1 ||
            pack.dst.bank!=usse::RegisterBank::Temp || pack.dest_mask!=0xF || pack.no_schedule)
            failures += fail("typed F32x4 to F16x4 conversion did not lower to validated VPCK form");
    }

    {
        TypedProgram program;
        const auto source = program.input<TypedType::F32x4>(0);
        const auto dst = program.make_value<TypedType::F16x4>();
        program.emit<TypedOpcode::FloatConvert>(1, dst, source);
        MachineCompileResult result;
        if (compile_typed_program(program, result))
            failures += fail("unvalidated typed float conversion subop was accepted");
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32x2>(0);
        const auto rhs = program.uniform<TypedType::F32x2>(0);
        const auto dst = program.make_value<TypedType::F32x2>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Add), dst, lhs, rhs);
        MachineCompileResult result;
        usse::V32NmadSemantic add{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0], &add) ||
            add.op!=usse::VectorOp::Add || add.dest_mask!=0x3 ||
            add.src1.bank!=usse::RegisterBank::PrimaryAttribute ||
            add.src2.bank!=usse::RegisterBank::SecondaryAttribute)
            failures += fail("typed float2 arithmetic did not lower to masked V32NMAD");
    }

    {
        TypedProgram program;
        const auto lhs = program.input<TypedType::F32x3>(0);
        const auto rhs = program.input<TypedType::F32x3>(1);
        const auto dst = program.make_value<TypedType::F32x3>();
        program.emit<TypedOpcode::FloatBinary>(static_cast<uint8_t>(TypedFloatOp::Mul), dst, lhs, rhs);
        MachineCompileResult result;
        usse::V32NmadSemantic mul{};
        if (!compile_typed_program(program, result) || result.words.size()!=1 ||
            !usse::decode_v32nmad_semantic(result.words[0], &mul) ||
            mul.op!=usse::VectorOp::Mul || mul.dest_mask!=0x7 ||
            mul.src1.bank!=usse::RegisterBank::PrimaryAttribute || mul.src1.num!=0 ||
            mul.src2.bank!=usse::RegisterBank::PrimaryAttribute || mul.src2.num!=2)
            failures += fail("typed float3 arithmetic did not lower to masked V32NMAD");
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
        const auto source=make_f32_if_else_spirv();
        TypedProgram program;
        std::string error;
        if (!spirv_cross_to_typed_fragment(source,"main",program,error)) {
            failures += fail("SPIRV-Cross rejected structured F32 if/else fixture");
        } else {
            MachineCompileResult result;
            if (!compile_typed_program(program,result) || result.words.empty() ||
                result.words[0]!=0x48088a81a0038002ULL) {
                failures += fail("SPIR-V F32 compare/if did not reproduce oracle VTST anchor");
            }
        }
    }

    {
        const auto source = make_u32_if_else_spirv();
        TypedProgram program;
        std::string error;
        if (!spirv_cross_to_typed_fragment(source, "main", program, error)) {
            failures += fail("SPIRV-Cross rejected structured U32 if/else fixture");
        } else {
            bool conditional=false, jump=false;
            for (const auto &instruction : program.instructions()) {
                conditional |= instruction.opcode()==TypedOpcode::Branch;
                jump |= instruction.opcode()==TypedOpcode::Jump;
            }
            MachineCompileResult result;
            if (!conditional || !jump || !compile_typed_program(program,result)) {
                failures += fail("SPIR-V structured if/else did not reach Machine IR branches");
            } else {
                size_t branches=0;
                for (uint64_t word : result.words)
                    if (usse::classify_control(word)==usse::ControlClass::Branch) ++branches;
                if (branches<4)
                    failures += fail("SPIR-V if/else emitted too few control-flow branches");
            }
        }
    }

    {
        const auto source = make_float_extinst_spirv();
        TypedShader shader(TypedStage::Fragment);
        std::string error;
        if (!spirv_cross_to_typed_shader(source, TypedStage::Fragment, "main", shader, error)) {
            failures += fail("SPIRV-Cross rejected validated GLSL.std.450 float fixture");
        } else {
            bool splat=false, abs=false, min=false, max=false;
            for (const auto &instruction : shader.program().instructions()) {
                splat |= instruction.opcode()==TypedOpcode::FloatSplat;
                abs |= instruction.opcode()==TypedOpcode::FloatUnary &&
                    instruction.subop()==static_cast<uint8_t>(TypedFloatUnaryOp::Abs);
                min |= instruction.opcode()==TypedOpcode::FloatBinary &&
                    instruction.subop()==static_cast<uint8_t>(TypedFloatOp::Min);
                max |= instruction.opcode()==TypedOpcode::FloatBinary &&
                    instruction.subop()==static_cast<uint8_t>(TypedFloatOp::Max);
            }
            if (!splat || !abs || !min || !max)
                failures += fail("SPIRV-Cross did not preserve validated splat/abs/min/max Typed IR operations");
            IrCompileResult result;
            if (!compile_typed_shader(shader, result))
                failures += fail("GLSL.std.450 TypedShader did not compile through Machine IR/GXP");
        }

        auto bad = source;
        for (size_t offset=5; offset<bad.size();) {
            const uint16_t count=static_cast<uint16_t>(bad[offset]>>16);
            const uint16_t op=static_cast<uint16_t>(bad[offset]);
            if (!count || offset+count>bad.size()) break;
            if (op==79 && count==9) { bad[offset+6]=1; break; }
            offset+=count;
        }
        TypedShader rejected(TypedStage::Fragment);
        if (spirv_cross_to_typed_shader(bad, TypedStage::Fragment, "main", rejected, error))
            failures += fail("SPIRV-Cross accepted an unvalidated vector shuffle pattern");
    }

    {
        const auto source = make_float_convert_spirv();
        TypedShader shader(TypedStage::Fragment);
        std::string error;
        if (!spirv_cross_to_typed_shader(source, TypedStage::Fragment, "main", shader, error)) {
            failures += fail("SPIRV-Cross rejected validated F32x4 to F16x4 conversion fixture");
        } else {
            bool convert=false;
            for (const auto &instruction : shader.program().instructions())
                convert |= instruction.opcode()==TypedOpcode::FloatConvert &&
                    instruction.subop()==static_cast<uint8_t>(TypedFloatConvertOp::F32x4ToF16x4);
            if (!convert) failures += fail("SPIRV-Cross did not emit Typed IR float conversion");
        }
    }

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
