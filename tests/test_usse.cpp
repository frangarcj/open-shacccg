#include "usse/usse.hpp"

#include <cstdio>

namespace {
int fail(const char *msg) {
    std::fprintf(stderr, "test_usse: %s\n", msg);
    return 1;
}
}

int test_usse() {
    using namespace vsc::usse;
    int failures = 0;

    struct Case { uint64_t word; uint8_t major; MajorClass cls; };
    const Case cases[] = {
        {0xfa44070000000000ULL, 0x1f, MajorClass::Control},
        {0x38800422c5000000ULL, 0x07, MajorClass::Vmov},
        {0x40840d7ea0198002ULL, 0x08, MajorClass::Vpck},
        {0x48880185b007c006ULL, 0x09, MajorClass::Vtst},
        {0x50810009e0400600ULL, 0x0a, MajorClass::Vbw},
        {0x08c51f889f240001ULL, 0x01, MajorClass::V32Nmad},
        {0x18b18f80cf411100ULL, 0x03, MajorClass::VectorMadDot},
    };
    for (const auto &c : cases) {
        if (major_opcode(c.word) != c.major) failures += fail("wrong major opcode extraction");
        if (classify_major(c.word) != c.cls) failures += fail("wrong USSE2 major class");
        if (!major_class_name(c.cls) || !*major_class_name(c.cls)) failures += fail("missing USSE2 class name");
    }

    if (classify_major(0x8800000000000000ULL) != MajorClass::Unknown)
        failures += fail("unknown major opcode misclassified");

    if (classify_control(0xfa44070000000000ULL) != ControlClass::Phase)
        failures += fail("PHAS control form not recognized");
    if (classify_control(0xf800094000000000ULL) != ControlClass::Nop)
        failures += fail("NOP control form not recognized");
    if (classify_control(0xfb275000a0200000ULL) != ControlClass::Emit)
        failures += fail("EMIT control form not recognized");
    if (classify_control(0xf9300406f0000408ULL) != ControlClass::Kill)
        failures += fail("KILL control form not recognized");
    if (classify_control(0xf90000400000000cULL) != ControlClass::Branch ||
        classify_control(0xf8000040000ffffaULL) != ControlClass::Branch)
        failures += fail("oracle BR control forms not recognized");

    // Raw VMOV codec: preserve every field of a known-good public instruction.
    VmovFields vm{};
    const uint64_t known_vmov = 0x38800422c5000000ULL;
    if (!decode_vmov(known_vmov, &vm)) failures += fail("known VMOV did not decode");
    uint64_t vm_roundtrip = 0;
    if (!encode_vmov(vm, &vm_roundtrip) || vm_roundtrip != known_vmov)
        failures += fail("VMOV raw field roundtrip mismatch");
    if (vm.data_type != 4 || vm.src0_swizzle != 4 || vm.dest_bank != 2 ||
        vm.src1_bank != 3 || vm.dest_mask != 5)
        failures += fail("VMOV field positions mismatch");
    vm.dest_num = 64;
    if (encode_vmov(vm, &vm_roundtrip)) failures += fail("VMOV accepted out-of-range register");

    // VPCK raw codec: exercise distinct public forms, including the end bit,
    // format changes, register-bank changes and component selectors.
    const uint64_t vpck_words[] = {
        0x40840d7ea0198002ULL,
        0x40800d7ea0198002ULL,
        0x40c00dbcaf998002ULL,
        0x40810d7e2019bc00ULL,
        0x40c00d9caf818002ULL,
        0x40c00dbcffb9860eULL,
    };
    for (uint64_t known : vpck_words) {
        VpckFields vp{};
        uint64_t roundtrip = 0;
        if (!decode_vpck(known, &vp) || !encode_vpck(vp, &roundtrip) || roundtrip != known)
            failures += fail("VPCK raw field roundtrip mismatch");
    }
    VpckFields vp{};
    decode_vpck(0x40840d7ea0198002ULL, &vp);
    if (vp.src_format != 6 || vp.dest_format != 5 || vp.dest_mask != 15 ||
        vp.dest_bank != 2 || !vp.end || vp.component3 != 3 || vp.src2_num != 1)
        failures += fail("VPCK field positions mismatch");
    vp.dest_num = 128;
    uint64_t vp_bad = 0;
    if (encode_vpck(vp, &vp_bad)) failures += fail("VPCK accepted out-of-range register");

    // Both V32NMAD words currently present in the public corpus must preserve
    // all encoded fields bit-for-bit.
    const uint64_t nmad_words[] = {
        0x08a44784cf04003cULL,
        0x08c51f889f240001ULL,
    };
    for (uint64_t known : nmad_words) {
        V32NmadFields nm{};
        uint64_t roundtrip = 0;
        if (!decode_v32nmad(known, &nm) || !encode_v32nmad(nm, &roundtrip) || roundtrip != known)
            failures += fail("V32NMAD raw field roundtrip mismatch");
    }

    // Matrix path in color_v/texture_v: all four major-0x03 words are VMAD,
    // not VDP. Preserve their operand fields exactly.
    const uint64_t vmad_words[] = {
        0x18b18f80cf411100ULL,
        0x18b18f80cf451102ULL,
        0x18b18181c0091104ULL,
        0x18b18181c04ad105ULL,
    };
    for (uint64_t known : vmad_words) {
        VmadFields mad{};
        uint64_t roundtrip = 0;
        if (!decode_vmad(known, &mad) || !encode_vmad(mad, &roundtrip) || roundtrip != known)
            failures += fail("VMAD raw field roundtrip mismatch");
    }
    VmadFields mad{};
    decode_vmad(vmad_words[0], &mad);
    if (mad.write_mask != 15 || mad.dest_num != 61 || mad.dest_bank != 0 || mad.src1_bank != 3)
        failures += fail("VMAD field positions mismatch");

    // Real scalar U32 CMP forms exercise VTST fields, destination predicates,
    // and instruction predication independently of the libvita2d corpus.
    const uint64_t vtst_words[] = {
        0x48880185b007c006ULL,
        0x48880181b007c008ULL,
        0x4d880181b007c006ULL,
        0x4e880185b007c007ULL,
    };
    for (uint64_t known : vtst_words) {
        VtstFields tst{};
        uint64_t roundtrip = 0;
        if (!decode_vtst(known, &tst) || !encode_vtst(tst, &roundtrip) || roundtrip != known)
            failures += fail("VTST raw field roundtrip mismatch");
    }
    VtstFields tst{};
    decode_vtst(vtst_words[0], &tst);
    if (tst.pred != 0 || !tst.skip_invalid || !tst.dest_ext || tst.zero_test != 1 ||
        tst.sign_test != 0 || tst.predicate_destination != 1 || tst.src1_bank != 2 ||
        tst.src2_bank != 3 || tst.alu_select != 1 || tst.alu_op != 15 ||
        tst.src1_num != 0 || tst.src2_num != 6)
        failures += fail("VTST field positions mismatch");

    const uint64_t vbw_words[] = {
        0x50810009e0400600ULL,
        0x50810009e0000000ULL,
    };
    for (uint64_t known : vbw_words) {
        VbwFields bw{};
        uint64_t roundtrip = 0;
        if (!decode_vbw(known,&bw) || !encode_vbw(bw,&roundtrip) || roundtrip!=known)
            failures += fail("VBW raw field roundtrip mismatch");
    }
    VbwFields bw{};
    decode_vbw(vbw_words[0],&bw);
    if (bw.op1!=2 || !bw.op2 || bw.dest_bank!=1 || bw.src1_bank!=3 ||
        bw.src2_bank!=2 || !bw.src2_ext || bw.dest_num!=2 || bw.src1_num!=12 ||
        bw.src2_num!=0 || bw.src2_select!=0)
        failures += fail("VBW field positions mismatch");

    const uint64_t kill_words[] = {
        0xf9300406f0000408ULL,
        0xf9300406f0001c38ULL,
        0xf9300406f000060cULL,
    };
    for (uint64_t known : kill_words) {
        KillFields kill{};
        uint64_t roundtrip=0;
        if (!decode_kill(known,&kill) || !encode_kill(kill,&roundtrip) || roundtrip!=known)
            failures += fail("KILL raw field roundtrip mismatch");
        if (kill.short_predicate!=2)
            failures += fail("KILL short predicate mismatch");
    }

    // Branch words captured from the original SceShaccCg 1.6.5 oracle.
    // The loop back-edge establishes signed offset direction independently of
    // the two forward if/else edges.
    const uint64_t branch_words[] = {
        0xf90000400000000cULL, // P0, +12
        0xf80000400000000bULL, // always, +11
        0xfd00004000000006ULL, // !P0, +6
        0xf8000040000ffffaULL, // always, -6
    };
    for (uint64_t known : branch_words) {
        BranchFields br{};
        uint64_t roundtrip=0;
        if (!decode_branch(known,&br) || !encode_branch(br,&roundtrip) || roundtrip!=known)
            failures += fail("BR raw field roundtrip mismatch");
    }
    BranchFields br{};
    if (!decode_branch(branch_words[0],&br) || br.pred!=1 || br.offset!=12)
        failures += fail("BR raw forward fields mismatch");
    if (!decode_branch(branch_words[3],&br) || br.pred!=0 || br.offset!=0xffffa)
        failures += fail("BR raw backward fields mismatch");

    // Semantic bank mapping is intentionally context-sensitive.
    uint8_t bank_sel = 0; bool bank_ext = false;
    if (!encode_dest_bank(RegisterBank::Output, &bank_sel, &bank_ext) || bank_sel != 1 || bank_ext)
        failures += fail("semantic OUTPUT destination bank mapping mismatch");
    if (!encode_src1_bank(RegisterBank::SecondaryAttribute, &bank_sel, &bank_ext) || bank_sel != 3 || bank_ext)
        failures += fail("semantic SA source bank mapping mismatch");
    if (!encode_src1_bank(RegisterBank::Immediate, &bank_sel, &bank_ext) || bank_sel != 2 || !bank_ext)
        failures += fail("semantic immediate source bank mapping mismatch");

    // Rebuild a known public F32 vertex VMOV from semantic operands only:
    // OUTPUT[2].xy <- PRIMARY_ATTRIBUTE[2].xyzw.
    VmovSemantic sem{};
    sem.dst = {RegisterBank::Output, 2};
    sem.src = {RegisterBank::PrimaryAttribute, 2};
    sem.data_type = DataType::F32;
    sem.dest_mask = 3;
    sem.swizzle = 4;
    sem.repeat_count = 0;
    sem.skip_invalid = true;
    sem.no_schedule = true;
    uint64_t semantic_word = 0;
    if (!encode_vmov_semantic(sem, &semantic_word) || semantic_word != 0x38800d2183080080ULL)
        failures += fail("semantic VMOV builder mismatch");
    VmovSemantic sem_dec{};
    if (!decode_vmov_semantic(semantic_word, &sem_dec) ||
        sem_dec.dst.bank != RegisterBank::Output || sem_dec.dst.num != 2 ||
        sem_dec.src.bank != RegisterBank::PrimaryAttribute || sem_dec.src.num != 2 ||
        sem_dec.data_type != DataType::F32 || sem_dec.dest_mask != 3 || sem_dec.swizzle != 4)
        failures += fail("semantic VMOV decode mismatch");

    // Predication uses the same semantic path rather than forcing clients to
    // manipulate the raw three-bit predicate field.
    VmovSemantic pred_move{};
    pred_move.dst={RegisterBank::Output,1};
    pred_move.src={RegisterBank::SecondaryAttribute,1};
    pred_move.predicate=Predicate::NotP0;
    pred_move.data_type=DataType::F32;
    pred_move.dest_mask=1;
    pred_move.swizzle=0;
    uint64_t pred_move_word=0;
    if (!encode_vmov_semantic(pred_move,&pred_move_word) || pred_move_word!=0x3d800501c1040040ULL)
        failures += fail("predicated semantic VMOV mismatch");
    if (!decode_vmov_semantic(pred_move_word,&sem_dec) || sem_dec.predicate!=Predicate::NotP0)
        failures += fail("predicated semantic VMOV decode mismatch");

    // Semantic VPCK: rebuild the public F32->F16 identity pack exactly.
    VpckSemantic pack{};
    pack.dst = {RegisterBank::PrimaryAttribute, 0};
    pack.src1 = {RegisterBank::PrimaryAttribute, 0};
    pack.src2 = {RegisterBank::PrimaryAttribute, 1};
    pack.src_format = PackFormat::F32;
    pack.dst_format = PackFormat::F16;
    pack.dest_mask = 0xF;
    pack.components[0]=0; pack.components[1]=1; pack.components[2]=2; pack.components[3]=3;
    pack.skip_invalid = true;
    pack.no_schedule = false;
    pack.end = false;
    uint64_t pack_word = 0;
    if (!encode_vpck_semantic(pack, &pack_word) || pack_word != 0x40800d7ea0198002ULL)
        failures += fail("semantic VPCK builder mismatch");
    VpckSemantic pack_dec{};
    if (!decode_vpck_semantic(pack_word, &pack_dec) ||
        pack_dec.dst.bank != RegisterBank::PrimaryAttribute || pack_dec.dst.num != 0 ||
        pack_dec.src1.bank != RegisterBank::PrimaryAttribute || pack_dec.src2.num != 1 ||
        pack_dec.src_format != PackFormat::F32 || pack_dec.dst_format != PackFormat::F16 ||
        pack_dec.components[3] != 3)
        failures += fail("semantic VPCK decode mismatch");

    // Semantic V32NMAD: texture_tint_f is an F32 multiply of the sampled
    // TEMP value by the tint uniform in secondary attributes.
    V32NmadSemantic mul{};
    mul.op = VectorOp::Mul;
    mul.dst = {RegisterBank::Temp, 60};
    mul.src1 = {RegisterBank::SecondaryAttribute, 0};
    mul.src2 = {RegisterBank::Temp, 60};
    mul.dest_mask = 0xF;
    mul.src1_swizzle = {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}};
    mul.src2_swizzle = mul.src1_swizzle;
    mul.skip_invalid = true;
    mul.no_schedule = false;
    uint64_t mul_word=0;
    if (!encode_v32nmad_semantic(mul,&mul_word) || mul_word != 0x08a44784cf04003cULL)
        failures += fail("semantic V32NMAD multiply builder mismatch");
    V32NmadSemantic mul_dec{};
    if (!decode_v32nmad_semantic(mul_word,&mul_dec) || mul_dec.op != VectorOp::Mul ||
        mul_dec.dst.bank != RegisterBank::Temp || mul_dec.src1.bank != RegisterBank::SecondaryAttribute ||
        mul_dec.src2.bank != RegisterBank::Temp || mul_dec.src1.num != 0 || mul_dec.src2.num != 60)
        failures += fail("semantic V32NMAD decode mismatch");

    // clear_v uses the same multiply opcode with xy11 / yyyy swizzles.
    V32NmadSemantic clear_mul{};
    clear_mul.op=VectorOp::Mul; clear_mul.dst={RegisterBank::Temp,60};
    clear_mul.src1={RegisterBank::PrimaryAttribute,0}; clear_mul.src2={RegisterBank::Special,1};
    clear_mul.dest_mask=0xF;
    clear_mul.src1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::One,SwizzleChannel::One}};
    clear_mul.src2_swizzle={{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y}};
    clear_mul.skip_invalid=true; clear_mul.no_schedule=true;
    if (!encode_v32nmad_semantic(clear_mul,&mul_word) || mul_word != 0x08c51f889f240001ULL)
        failures += fail("semantic clear_v V32NMAD builder mismatch");

    // Semantic VMAD: reconstruct the complete four-instruction matrix path.
    const uint64_t matrix_words[] = {
        0x18b18f80cf411100ULL,0x18b18f80cf451102ULL,
        0x18b18181c0091104ULL,0x18b18181c04ad105ULL
    };
    VmadSemantic ms[4]{};
    for (auto &m: ms) {
        m.gpi0=0; m.gpi1=1; m.vec4=true; m.repeat_mode=RepeatMode::Slmsi;
        m.skip_invalid=true; m.src1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}};
        m.gpi1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}};
    }
    ms[0].dst={RegisterBank::Temp,61}; ms[0].src1={RegisterBank::SecondaryAttribute,0}; ms[0].write_mask=0xF; ms[0].no_schedule=true;
    ms[0].gpi0_swizzle={{SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X}};
    ms[1]=ms[0]; ms[1].src1.num=2; ms[1].gpi0_swizzle={{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y}};
    ms[2]=ms[0]; ms[2].dst={RegisterBank::Output,0}; ms[2].src1.num=4; ms[2].write_mask=3; ms[2].no_schedule=false;
    ms[2].gpi0_swizzle={{SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z}};
    ms[3]=ms[2]; ms[3].dst.num=1; ms[3].src1.num=5;
    ms[3].gpi1_swizzle={{SwizzleChannel::Z,SwizzleChannel::W,SwizzleChannel::Z,SwizzleChannel::W}};
    for (int k=0;k<4;k++) {
        uint64_t w=0; if (!encode_vmad_semantic(ms[k],&w) || w!=matrix_words[k]) failures += fail("semantic VMAD matrix builder mismatch");
        VmadSemantic d{}; if (!decode_vmad_semantic(matrix_words[k],&d) || d.dst.bank!=ms[k].dst.bank || d.src1.bank!=RegisterBank::SecondaryAttribute || d.gpi0!=0 || d.gpi1!=1)
            failures += fail("semantic VMAD decode mismatch");
    }

    // Semantic VTST: PA0.x == SA6.x -> P1. This exact word is independently
    // observed in a real shader trace and anchors the U32 compare form.
    VtstSemantic cmp{};
    cmp.lhs={RegisterBank::PrimaryAttribute,0};
    cmp.rhs={RegisterBank::SecondaryAttribute,6};
    cmp.op=CompareOp::Equal;
    cmp.predicate_destination=1;
    cmp.component=0;
    uint64_t cmp_word=0;
    if (!encode_vtst_semantic(cmp,&cmp_word) || cmp_word!=0x48880185b007c006ULL)
        failures += fail("semantic VTST U32 equality mismatch");
    VtstSemantic cmp_dec{};
    if (!decode_vtst_semantic(cmp_word,&cmp_dec) || cmp_dec.op!=CompareOp::Equal ||
        cmp_dec.predicate_destination!=1 || cmp_dec.lhs.bank!=RegisterBank::PrimaryAttribute ||
        cmp_dec.rhs.bank!=RegisterBank::SecondaryAttribute || cmp_dec.rhs.num!=6)
        failures += fail("semantic VTST U32 equality decode mismatch");
    cmp.predicate=Predicate::NotP0;
    cmp.predicate_destination=0;
    if (!encode_vtst_semantic(cmp,&cmp_word) || cmp_word!=0x4d880181b007c006ULL)
        failures += fail("predicated semantic VTST mismatch");

    // Semantic VBW: the real shader trace uses OR with an immediate zero as a
    // scalar U32 copy. Keep the immediate encoding generic but fail if a U32
    // constant cannot be represented by the USSE rotated-16-bit form.
    VbwSemantic bw_or{};
    bw_or.op=BitwiseOp::Or;
    bw_or.dst={RegisterBank::Output,2};
    bw_or.src1={RegisterBank::SecondaryAttribute,12};
    bw_or.src2_is_immediate=true;
    bw_or.immediate=0;
    uint64_t bw_word=0;
    if (!encode_vbw_semantic(bw_or,&bw_word) || bw_word!=0x50810009e0400600ULL)
        failures += fail("semantic VBW OR-immediate mismatch");
    VbwSemantic bw_dec{};
    if (!decode_vbw_semantic(bw_word,&bw_dec) || bw_dec.op!=BitwiseOp::Or ||
        bw_dec.dst.bank!=RegisterBank::Output || bw_dec.dst.num!=2 ||
        bw_dec.src1.bank!=RegisterBank::SecondaryAttribute || bw_dec.src1.num!=12 ||
        !bw_dec.src2_is_immediate || bw_dec.immediate!=0)
        failures += fail("semantic VBW OR-immediate decode mismatch");

    // Exercise a non-zero rotated immediate independently of the trace word.
    bw_or.immediate=0x00ff0000u;
    if (!encode_vbw_semantic(bw_or,&bw_word) || !decode_vbw_semantic(bw_word,&bw_dec) ||
        bw_dec.immediate!=0x00ff0000u)
        failures += fail("semantic VBW rotated immediate roundtrip mismatch");

    KillSemantic kill{};
    kill.predicate=Predicate::P1;
    uint64_t kill_word=0;
    if (!encode_kill_semantic(kill,&kill_word) || kill_word!=0xf9300406f0000000ULL)
        failures += fail("semantic KILL canonical encoding mismatch");
    KillSemantic kill_dec{};
    if (!decode_kill_semantic(kill_words[0],&kill_dec) || kill_dec.predicate!=Predicate::P1)
        failures += fail("semantic KILL decode mismatch");
    kill.predicate=Predicate::P2;
    if (encode_kill_semantic(kill,&kill_word))
        failures += fail("KILL accepted predicate unavailable in short form");

    BranchSemantic branch{};
    uint64_t branch_word=0;
    branch.predicate=Predicate::P0; branch.offset=12;
    if (!encode_branch_semantic(branch,&branch_word) || branch_word!=branch_words[0])
        failures += fail("semantic forward P0 BR mismatch");
    branch.predicate=Predicate::NotP0; branch.offset=6;
    if (!encode_branch_semantic(branch,&branch_word) || branch_word!=branch_words[2])
        failures += fail("semantic forward !P0 BR mismatch");
    branch.predicate=Predicate::Always; branch.offset=-6;
    if (!encode_branch_semantic(branch,&branch_word) || branch_word!=branch_words[3])
        failures += fail("semantic backward BR mismatch");
    BranchSemantic branch_dec{};
    if (!decode_branch_semantic(branch_words[3],&branch_dec) ||
        branch_dec.predicate!=Predicate::Always || branch_dec.offset!=-6)
        failures += fail("semantic backward BR decode mismatch");
    branch.offset=1<<19;
    if (encode_branch_semantic(branch,&branch_word))
        failures += fail("BR accepted positive offset outside signed-20 range");
    branch.offset=0; branch.predicate=Predicate::P1;
    if (encode_branch_semantic(branch,&branch_word))
        failures += fail("BR accepted predicate not yet validated by oracle");

    // End-to-end instruction-stream regression: construct texture_v entirely
    // through semantic builders + fixed control encoders, then compare against
    // the public known-good primary program word-for-word.
    const uint64_t texture_v_known[] = {
        0xfa44070000000000ULL,0x38800d2183080080ULL,0x40c00d9caf818002ULL,
        0x40c00dbcffb9860eULL,0x18b18f80cf411100ULL,0x18b18f80cf451102ULL,
        0x18b18181c0091104ULL,0x18b18181c04ad105ULL,0xfb275000a0200000ULL
    };
    uint64_t texture_v_built[9]{};
    Instruction ctl{}; ctl.opcode=Opcode::Phase; encode(ctl,&texture_v_built[0]);
    VmovSemantic tvmov{}; tvmov.dst={RegisterBank::Output,2}; tvmov.src={RegisterBank::PrimaryAttribute,2};
    tvmov.data_type=DataType::F32; tvmov.dest_mask=3; tvmov.swizzle=4; tvmov.skip_invalid=true; tvmov.no_schedule=true;
    if (!encode_vmov_semantic(tvmov,&texture_v_built[1])) failures += fail("texture_v semantic VMOV failed");
    VpckSemantic tvp0{}; tvp0.dst={RegisterBank::Temp,124}; tvp0.src1={RegisterBank::PrimaryAttribute,0}; tvp0.src2={RegisterBank::PrimaryAttribute,1};
    tvp0.src_format=PackFormat::F32; tvp0.dst_format=PackFormat::F32; tvp0.dest_mask=7; tvp0.skip_invalid=true; tvp0.no_schedule=true;
    if (!encode_vpck_semantic(tvp0,&texture_v_built[2])) failures += fail("texture_v first semantic VPCK failed");
    VpckSemantic tvp1{}; tvp1.dst={RegisterBank::Temp,125}; tvp1.src1={RegisterBank::SecondaryAttribute,6}; tvp1.src2={RegisterBank::SecondaryAttribute,7};
    tvp1.src_format=PackFormat::F32; tvp1.dst_format=PackFormat::F32; tvp1.dest_mask=15; tvp1.skip_invalid=true; tvp1.no_schedule=true;
    if (!encode_vpck_semantic(tvp1,&texture_v_built[3])) failures += fail("texture_v second semantic VPCK failed");
    for (int k=0;k<4;k++) if (!encode_vmad_semantic(ms[k],&texture_v_built[4+k])) failures += fail("texture_v semantic VMAD failed");
    ctl.opcode=Opcode::Emit; encode(ctl,&texture_v_built[8]);
    for (int k=0;k<9;k++) if (texture_v_built[k]!=texture_v_known[k]) failures += fail("full texture_v semantic stream mismatch");

    // color_v differs from texture_v only in the repeated VMOV at instruction 1.
    tvmov.repeat_count=1;
    uint64_t color_move=0;
    if (!encode_vmov_semantic(tvmov,&color_move) || color_move!=0x38801d2183080080ULL)
        failures += fail("color_v semantic repeated VMOV mismatch");

    uint64_t encoded = 0;
    Instruction ins;
    ins.opcode = Opcode::Phase;
    if (!encode(ins, &encoded) || encoded != 0xfa44070000000000ULL)
        failures += fail("PHAS encoder mismatch");
    ins.opcode = Opcode::Nop;
    if (!encode(ins, &encoded) || encoded != 0xf800094000000000ULL)
        failures += fail("NOP encoder mismatch");
    ins.opcode = Opcode::Emit;
    if (!encode(ins, &encoded) || encoded != 0xfb275000a0200000ULL)
        failures += fail("EMIT encoder mismatch");
    ins.opcode = Opcode::Mov;
    if (encode(ins, &encoded))
        failures += fail("unsupported operand-bearing opcode was guessed");

    return failures;
}
