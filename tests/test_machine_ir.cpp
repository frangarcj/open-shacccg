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
        const std::vector<MachineLiveRange> ranges = {
            {0, 4, MachineType::F32, MachineRegisterClass::FloatTemp, MachineRegisterOrder::Low, 2},
            {1, 3, MachineType::F32, MachineRegisterClass::FloatTemp, MachineRegisterOrder::Low, 2},
            {4, 6, MachineType::F32, MachineRegisterClass::FloatTemp, MachineRegisterOrder::Low, 2},
            {0, 6, MachineType::F32, MachineRegisterClass::Gpi, MachineRegisterOrder::Low, 1},
            {0, 6, MachineType::F32, MachineRegisterClass::Gpi, MachineRegisterOrder::Low, 1},
            {6, 8, MachineType::F32, MachineRegisterClass::FloatTemp, MachineRegisterOrder::High, 2},
            {0, 6, MachineType::F32, MachineRegisterClass::VmadAccumulator, MachineRegisterOrder::Low, 1},
            {8, 10, MachineType::F16, MachineRegisterClass::FloatTemp, MachineRegisterOrder::Low, 1},
        };
        std::vector<usse::RegisterRef> regs;
        std::string error;
        if (!allocate_machine_live_ranges(ranges, regs, error)) {
            failures += fail("bank-aware float register allocation failed");
        } else {
            if (regs.size() != ranges.size() || regs[0].num != 4 || regs[1].num != 6 || regs[2].num != 4)
                failures += fail("float TEMP pair allocation/reuse was not deterministic");
            if (machine_gpi_index(regs[3]) != 0 || machine_gpi_index(regs[4]) != 1)
                failures += fail("GPI aliases were not allocated from the validated TEMP124 range");
            if (regs[5].num != 60) failures += fail("high float TEMP preference did not select TEMP60");
            if (regs[6].num != 61) failures += fail("VMAD accumulator class did not select TEMP61");
            if (regs[7].num != 4) failures += fail("F16 TEMP allocation did not use the float bank policy");
        }
    }

    {
        MachineProgram program;
        const auto target = program.make_label();
        if (target.kind()==MachineOperandKind::None ||
            !program.branch(target, MachineOperand::physical_predicate(0))) {
            failures += fail("could not construct forward P0 machine branch");
        } else {
            for (int i=0;i<11;i++)
                if (!program.emit<MachineOpcode::Nop>()) failures += fail("could not append branch padding NOP");
            if (!program.bind_label(target)) failures += fail("could not bind forward branch label");
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=12 ||
                result.words[0]!=0xf90000400000000cULL)
                failures += fail("machine forward P0 branch did not reproduce oracle word");
        }
    }

    {
        const uint64_t expected2[]={
            0x308008008f800081ULL,0x308008088f800082ULL,
            0x3880052083f40000ULL,0x10a4418600040f7cULL,
        };
        const uint64_t expected3[]={
            0x308008008f800101ULL,0x308008088f800102ULL,0x308008008f800184ULL,
            0x40800d9cafa18002ULL,0x10a4438600040f7cULL,
        };
        const uint64_t expected4[]={
            0x308008008f800101ULL,0x308008088f800102ULL,
            0x308008008f800184ULL,0x308008088f800188ULL,
            0x40800dbcafb98002ULL,0x10a4478600040f7cULL,
        };
        for (uint8_t components=2;components<=4;++components) {
            MachineProgram program;
            const uint8_t rhs=components==2 ? 1 : 2;
            if (!program.emit<MachineOpcode::DivF32>(components,
                    program.physical(machine_fragment_output(0),MachineType::F16),
                    program.physical(machine_primary(0),MachineType::F32),
                    program.physical(machine_primary(rhs),MachineType::F32))) {
                failures += fail("could not construct oracle F32 division Machine IR");
                continue;
            }
            MachineCompileResult result;
            const uint64_t *expected=components==2 ? expected2 : (components==3 ? expected3 : expected4);
            const size_t count=static_cast<size_t>(components+2);
            if (!compile_machine_program(program,result) || result.words.size()!=count)
                failures += fail("oracle F32 division Machine IR did not compile");
            else for (size_t i=0;i<count;++i)
                if (result.words[i]!=expected[i]) failures += fail("oracle F32 division Machine word mismatch");
        }
    }

    {
        const uint64_t expected2[]={
            0x08c11f889f040041ULL,0x3880052083f40000ULL,0x10c0418a00047f7cULL,
        };
        const uint64_t expected3[]={
            0x40c00d9caf818002ULL,0x40800d9cafa18206ULL,0x10c0f38600047f3dULL,
        };
        for (uint8_t components=2;components<=3;++components) {
            MachineProgram program;
            const uint8_t rhs=components==2 ? 1 : 2;
            if (!program.emit<MachineOpcode::DotSplatF32>(components,
                    program.physical(machine_fragment_output(0),MachineType::F16),
                    program.physical(machine_primary(0),MachineType::F32),
                    program.physical(machine_primary(rhs),MachineType::F32))) {
                failures += fail("could not construct oracle narrow dot-splat Machine IR");
                continue;
            }
            MachineCompileResult result;
            const uint64_t *expected=components==2 ? expected2 : expected3;
            if (!compile_machine_program(program,result) || result.words.size()!=3)
                failures += fail("oracle narrow dot-splat Machine IR did not compile");
            else for (size_t i=0;i<3;++i)
                if (result.words[i]!=expected[i]) failures += fail("oracle narrow dot-splat Machine word mismatch");
        }
    }

    {
        MachineProgram program;
        const auto gpi0=program.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        if (!program.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F32),
                machine_pack_config(0xF,true,false),gpi0,
                program.physical(machine_primary(0),MachineType::F32),
                program.physical(machine_primary(1),MachineType::F32)) ||
            !program.emit<MachineOpcode::VmadUniformMat4>(0,
                program.physical(machine_vertex_output(0),MachineType::F32),gpi0)) {
            failures += fail("could not construct oracle uniform-mat4 Machine sequence");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=2 ||
                result.words[0]!=0x40800dbcaf998002ULL ||
                result.words[1]!=0x18903081c011a200ULL)
                failures += fail("uniform-mat4 Machine sequence did not reproduce oracle words");
        }
    }

    {
        MachineProgram program;
        const uint8_t wzyx=static_cast<uint8_t>(3u | (2u<<2) | (1u<<4));
        if (!program.emit_config<MachineOpcode::PackSwizzle>(wzyx,machine_pack_config(0xF,true,false),
                program.physical(machine_fragment_output(0),MachineType::F16),
                program.physical(machine_primary(0),MachineType::F32),
                program.physical(machine_primary(1),MachineType::F32))) {
            failures += fail("could not construct wzyx PackSwizzle Machine IR");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=1 ||
                result.words[0]!=0x40800d7ea0024083ULL)
                failures += fail("Machine PackSwizzle did not reproduce oracle wzyx VPCK");
        }
    }

    {
        MachineProgram program;
        const auto counter=program.make_value<MachineType::S32>();
        if (!program.emit<MachineOpcode::LoopCounterInit>(0,counter) ||
            !program.emit<MachineOpcode::LoopIncrement>(1,counter)) {
            failures += fail("could not construct oracle loop counter Machine IR");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=3 ||
                result.words[0]!=0x50810008e0000100ULL ||
                result.words[1]!=0xd08180042020c001ULL ||
                result.words[2]!=0xd09080040000c001ULL)
                failures += fail("Machine loop counter did not reproduce oracle VBW/I32MAD2 words");
        }
    }

    {
        MachineProgram program;
        const auto state=program.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,2);
        const auto source=program.physical(machine_primary(0),MachineType::F32);
        if (!program.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(0xF),state,source) ||
            !program.emit_config<MachineOpcode::MoveUpdate>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(0xF),state,source)) {
            failures += fail("could not construct mutable float Machine state");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=2 ||
                result.value_registers.empty() || result.value_registers[0].bank!=usse::RegisterBank::Temp)
                failures += fail("mutable float Machine state did not allocate/compile");
        }
    }

    {
        MachineProgram program;
        const auto head = program.make_label();
        if (head.kind()==MachineOperandKind::None || !program.bind_label(head)) {
            failures += fail("could not bind backward branch label");
        } else {
            for (int i=0;i<6;i++)
                if (!program.emit<MachineOpcode::Nop>()) failures += fail("could not append backward branch padding NOP");
            if (!program.branch(head)) failures += fail("could not construct backward machine branch");
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=7 ||
                result.words[6]!=0xf8000040000ffffaULL)
                failures += fail("machine backward branch did not reproduce oracle word");
        }
    }

    {
        MachineProgram program;
        const auto target=program.make_label();
        program.branch(target,MachineOperand::physical_predicate(1));
        program.bind_label(target);
        MachineCompileResult result;
        if (compile_machine_program(program,result))
            failures += fail("machine branch accepted unvalidated P1 predicate");
    }

    {
        MachineProgram program;
        const auto target=program.make_label();
        program.branch(target);
        MachineCompileResult result;
        if (compile_machine_program(program,result))
            failures += fail("machine branch accepted unbound label");
    }

    {
        MachineProgram program;
        const auto narrow = program.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp,1);
        const auto packed = program.make_value<MachineType::F16>(MachineRegisterClass::FloatTemp,1);
        if (!program.emit_config<MachineOpcode::Move>(static_cast<uint8_t>(usse::DataType::F32),
                machine_move_config(0xF),narrow,
                program.physical<MachineType::F32>(usse::RegisterBank::PrimaryAttribute,0)) ||
            !program.emit_config<MachineOpcode::PackValue>(
                machine_pack_subop(usse::PackFormat::F32,usse::PackFormat::F16),
                machine_pack_config(0xF,true,false),packed,narrow)) {
            failures += fail("could not construct invalid narrow pack-value program");
        } else {
            MachineCompileResult result;
            if (compile_machine_program(program,result))
                failures += fail("pack-value accepted an F32 source without a register pair");
        }
    }

    {
        MachineProgram program;
        const auto gpi0 = program.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto sample = program.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp, 2, MachineRegisterOrder::High);
        const auto tinted = program.make_value<MachineType::F32>(
            MachineRegisterClass::FloatTemp, 2, MachineRegisterOrder::High);
        if (!program.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
                machine_pack_config(0xF), gpi0,
                program.physical(machine_primary(0), MachineType::F32),
                program.physical(machine_primary(1), MachineType::F32)) ||
            !program.emit<MachineOpcode::DependentSample>(0, sample, gpi0) ||
            !program.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Mul),
                machine_vector_config(0xF), tinted,
                program.physical(machine_secondary(0), MachineType::F32), sample)) {
            failures += fail("could not construct texture-tint Machine IR sequence");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program, result)) {
                failures += fail("texture-tint Machine IR sequence did not compile");
            } else {
                if (result.words.size() != 2 || result.words[1] != 0x08a44784cf04003cULL)
                    failures += fail("machine V32NMAD did not reproduce texture-tint word");
                if (result.value_registers.size()!=3 || machine_gpi_index(result.value_registers[0])!=0 ||
                    result.value_registers[1].num!=60 || result.value_registers[2].num!=60)
                    failures += fail("dependent sample/result lifetimes did not coalesce to validated registers");
            }
        }
    }

    {
        MachineProgram program;
        const auto dst = program.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
        if (!program.emit_config<MachineOpcode::Vector>(static_cast<uint8_t>(usse::VectorOp::Add),
                0x1800, dst,
                program.physical(machine_primary(0), MachineType::F32),
                program.physical(machine_secondary(0), MachineType::F32))) {
            failures += fail("invalid vector config was rejected before compile-time validation");
        } else {
            MachineCompileResult result;
            if (compile_machine_program(program, result))
                failures += fail("unsupported machine vector config was accepted");
        }
    }

    {
        MachineProgram program;
        const auto a = program.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
        const auto b = program.make_value<MachineType::F32>(MachineRegisterClass::FloatTemp, 2);
        const auto pair = program.pair(a, b);
        program.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32), machine_pack_config(0xF),
            a, program.physical(machine_primary(0), MachineType::F32),
            program.physical(machine_primary(1), MachineType::F32));
        program.emit_config<MachineOpcode::Pack>(
            machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32), machine_pack_config(0xF),
            b, program.physical(machine_secondary(0), MachineType::F32),
            program.physical(machine_secondary(1), MachineType::F32));
        program.emit_config<MachineOpcode::Vmad>(0, machine_vmad_config(0xF),
            program.physical(machine_vertex_output(0), MachineType::F32),
            program.physical(machine_secondary(2), MachineType::F32), pair);
        MachineCompileResult result;
        if (compile_machine_program(program, result))
            failures += fail("VMAD accepted a pair that was not allocated to GPI aliases");
    }

    {
        MachineProgram program;
        const auto gpi0 = program.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto gpi1 = program.make_value<MachineType::F32>(MachineRegisterClass::Gpi);
        const auto pair = program.pair(gpi0, gpi1);
        if (!program.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
                machine_pack_config(7), gpi0,
                program.physical(machine_primary(0), MachineType::F32),
                program.physical(machine_primary(1), MachineType::F32)) ||
            !program.emit_config<MachineOpcode::Pack>(
                machine_pack_subop(usse::PackFormat::F32, usse::PackFormat::F32),
                machine_pack_config(15), gpi1,
                program.physical(machine_secondary(6), MachineType::F32),
                program.physical(machine_secondary(7), MachineType::F32))) {
            failures += fail("could not construct VMAD GPI staging");
        }
        for (uint8_t lane=0; lane<4; ++lane) {
            const auto dst = lane < 2 ?
                program.make_value<MachineType::F32>(MachineRegisterClass::VmadAccumulator) :
                program.physical(machine_vertex_output(static_cast<uint8_t>(lane-2)), MachineType::F32);
            const uint8_t src = lane < 2 ? static_cast<uint8_t>(lane*2) : static_cast<uint8_t>(lane+2);
            if (!program.emit_config<MachineOpcode::Vmad>(lane,
                    machine_vmad_config(lane<2 ? 0xF : 0x3, lane<2), dst,
                    program.physical(machine_secondary(src), MachineType::F32), pair))
                failures += fail("could not construct VMAD Machine IR lane");
        }
        MachineCompileResult result;
        const uint64_t expected[] = {
            0x18b18f80cf411100ULL, 0x18b18f80cf451102ULL,
            0x18b18181c0091104ULL, 0x18b18181c04ad105ULL,
        };
        if (!compile_machine_program(program, result)) {
            failures += fail("VMAD Machine IR sequence did not compile");
        } else if (result.words.size()!=6) {
            failures += fail("VMAD Machine IR emitted wrong instruction count");
        } else {
            for (size_t i=0;i<4;++i)
                if (result.words[i+2]!=expected[i]) failures += fail("machine VMAD word mismatch");
        }
    }

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

    {
        MachineProgram program;
        const auto predicate = program.make_predicate();
        const auto lhs = program.physical<MachineType::F32>(usse::RegisterBank::PrimaryAttribute,0);
        const auto rhs = program.physical<MachineType::F32>(usse::RegisterBank::PrimaryAttribute,2);
        if (!program.emit<MachineOpcode::Compare>(static_cast<uint8_t>(usse::CompareOp::Greater),
                                                  predicate,lhs,rhs)) {
            failures += fail("could not construct oracle F32 compare Machine IR");
        } else {
            MachineCompileResult result;
            if (!compile_machine_program(program,result) || result.words.size()!=1 ||
                result.words[0]!=0x48088a81a0038002ULL)
                failures += fail("machine F32 compare did not reproduce oracle VTST word");
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
