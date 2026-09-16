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
        {0xd08180042020c001ULL, 0x1a, MajorClass::I32Mad2},
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
    {
        NopSemantic nop{};
        uint64_t word=0;
        if (!encode_nop_semantic(nop,&word) || word!=0xf800094000000000ULL)
            failures += fail("default NOP semantic encoding mismatch");
        nop.no_schedule=false;
        nop.end=true;
        if (!encode_nop_semantic(nop,&word) || word!=0xf804014000000000ULL)
            failures += fail("secondary END NOP semantic encoding mismatch");
        NopSemantic decoded{};
        if (!decode_nop_semantic(word,&decoded) || decoded.no_schedule || !decoded.end)
            failures += fail("secondary END NOP semantic decode mismatch");
    }
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

    const uint64_t i32mad2_words[] = {
        0xd08180042020c001ULL, // TEMP1 = SA3 * TEMP0 + imm(1), sn=0
        0xd09080040000c001ULL, // TEMP0 feed-through from TEMP1, sn=1
        0xd08180042020c002ULL, // +2 differential oracle case
        0xd08180042020c003ULL, // +3 differential oracle case
    };
    for (uint64_t known : i32mad2_words) {
        I32Mad2Fields im{};
        uint64_t roundtrip=0;
        if (!decode_i32mad2(known,&im) || !encode_i32mad2(im,&roundtrip) || roundtrip!=known)
            failures += fail("I32MAD2 raw field roundtrip mismatch");
    }
    I32Mad2Fields im{};
    if (!decode_i32mad2(i32mad2_words[0],&im) || im.sn!=0 || im.dest_num!=1 ||
        im.src0_num!=3 || im.src1_num!=0 || im.src2_num!=1 || !im.src0_ext ||
        !im.src2_ext || im.src0_bank!=1 || im.src2_bank!=2)
        failures += fail("I32MAD2 oracle field positions mismatch");

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
    VmovSemantic cmp_alpha_move{};
    cmp_alpha_move.dst={RegisterBank::PrimaryAttribute,1};
    cmp_alpha_move.src={RegisterBank::Temp,0};
    cmp_alpha_move.predicate=Predicate::NotP0;
    cmp_alpha_move.data_type=DataType::F32;
    cmp_alpha_move.dest_mask=1;
    cmp_alpha_move.swizzle=0;
    if (!encode_vmov_semantic(cmp_alpha_move,&pred_move_word) || pred_move_word!=0x3d80050201040000ULL)
        failures += fail("Geometrizer CMP predicated alpha VMOV mismatch");
    VmovSemantic cmp_secondary{};
    cmp_secondary.dst={RegisterBank::PrimaryAttribute,1};
    cmp_secondary.src={RegisterBank::Special,1};
    cmp_secondary.data_type=DataType::F32;
    cmp_secondary.dest_mask=1;
    cmp_secondary.swizzle=1;
    cmp_secondary.end=true;
    uint64_t cmp_secondary_word=0;
    if (!encode_vmov_semantic(cmp_secondary,&cmp_secondary_word) || cmp_secondary_word!=0x3886050a41040040ULL ||
        !decode_vmov_semantic(cmp_secondary_word,&sem_dec) || !sem_dec.end)
        failures += fail("Geometrizer CMP secondary END VMOV mismatch");

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

    // F32 division oracle probes a/b vs b/a isolate the reciprocal VCOMP PA
    // source-pair field independently from the four scalar lane selectors.
    const uint64_t vcomp_pa0[] = {
        0x308008008f800001ULL,0x308008088f800002ULL,
        0x308008008f800084ULL,0x308008088f800088ULL,
    };
    const uint64_t vcomp_pa2[] = {
        0x308008008f800101ULL,0x308008088f800102ULL,
        0x308008008f800184ULL,0x308008088f800188ULL,
    };
    for (uint8_t lane=0;lane<4;++lane) {
        for (uint8_t pair=0;pair<2;++pair) {
            const uint64_t expected=pair ? vcomp_pa2[lane] : vcomp_pa0[lane];
            VcompRcpF32Fields raw{pair,lane};
            uint64_t word=0;
            if (!encode_vcomp_rcp_f32(raw,&word) || word!=expected)
                failures += fail("raw F32 reciprocal VCOMP oracle word mismatch");
            VcompRcpF32Fields decoded{};
            if (!decode_vcomp_rcp_f32(expected,&decoded) || decoded.source_pair!=pair || decoded.component!=lane)
                failures += fail("raw F32 reciprocal VCOMP decode mismatch");
            VcompRcpF32Semantic semantic{{RegisterBank::PrimaryAttribute,static_cast<uint8_t>(pair*2)},lane};
            if (!encode_vcomp_rcp_f32_semantic(semantic,&word) || word!=expected)
                failures += fail("semantic F32 reciprocal VCOMP oracle word mismatch");
        }
    }
    const uint64_t vcomp_pa1[]={0x308008008f800081ULL,0x308008088f800082ULL};
    for (uint8_t lane=0;lane<2;++lane) {
        VcompRcpF32Fields raw{0,lane,true};
        uint64_t word=0;
        if (!encode_vcomp_rcp_f32(raw,&word) || word!=vcomp_pa1[lane])
            failures += fail("raw packed-float2 reciprocal VCOMP oracle word mismatch");
        VcompRcpF32Semantic semantic{{RegisterBank::PrimaryAttribute,1},lane};
        if (!encode_vcomp_rcp_f32_semantic(semantic,&word) || word!=vcomp_pa1[lane])
            failures += fail("semantic packed-float2 reciprocal VCOMP oracle word mismatch");
        VcompRcpF32Semantic decoded{};
        if (!decode_vcomp_rcp_f32_semantic(word,&decoded) || decoded.src.num!=1 || decoded.component!=lane)
            failures += fail("semantic packed-float2 reciprocal VCOMP decode mismatch");
    }
    const uint64_t vcomp_scalar[]={0x308008008f800001ULL,0x308008088f800001ULL};
    for (uint8_t component=0;component<2;++component) {
        VcompRcpScalarF32Semantic semantic{{RegisterBank::PrimaryAttribute,0},component};
        uint64_t word=0;
        VcompRcpScalarF32Semantic decoded{};
        if (!encode_vcomp_rcp_scalar_f32_semantic(semantic,&word) || word!=vcomp_scalar[component] ||
            !decode_vcomp_rcp_scalar_f32_semantic(word,&decoded) || decoded.src.num!=0 ||
            decoded.component!=component)
            failures += fail("semantic packed-scalar reciprocal VCOMP oracle word mismatch");
    }
    {
        struct Probe {
            VcompF32Semantic semantic;
            uint64_t word;
        };
        const Probe probes[]={
            {{ComplexOp::Reciprocal,{RegisterBank::Temp,124},{RegisterBank::PrimaryAttribute,0},0,1,true,true,false},
             0x308008008f800001ULL},
            {{ComplexOp::Log2,{RegisterBank::PrimaryAttribute,0},{RegisterBank::PrimaryAttribute,0},0,1,true,false,false},
             0x3080040280000001ULL},
            {{ComplexOp::Log2,{RegisterBank::PrimaryAttribute,0},{RegisterBank::PrimaryAttribute,0},1,2,true,false,false},
             0x3080040a80000002ULL},
            {{ComplexOp::Log2,{RegisterBank::Temp,124},{RegisterBank::PrimaryAttribute,0},0,1,true,true,false},
             0x30800c008f800001ULL},
        };
        for (const auto &probe:probes) {
            uint64_t word=0;
            VcompF32Semantic decoded{};
            if (!encode_vcomp_f32_semantic(probe.semantic,&word) || word!=probe.word ||
                !decode_vcomp_f32_semantic(word,&decoded) || decoded.op!=probe.semantic.op ||
                decoded.dst.bank!=probe.semantic.dst.bank || decoded.dst.num!=probe.semantic.dst.num ||
                decoded.src.bank!=probe.semantic.src.bank || decoded.src.num!=probe.semantic.src.num ||
                decoded.src_component!=probe.semantic.src_component || decoded.dest_mask!=probe.semantic.dest_mask ||
                decoded.no_schedule!=probe.semantic.no_schedule || decoded.end!=probe.semantic.end)
                failures += fail("general F32 VCOMP reciprocal/Log2 oracle profile mismatch");
        }
        VcompF32Semantic sa_profile{ComplexOp::Reciprocal,{RegisterBank::Temp,0},
                                    {RegisterBank::SecondaryAttribute,0},0,1,true,false,false};
        uint64_t sa_word=0;
        VcompF32Semantic sa_decoded{};
        if (!encode_vcomp_f32_semantic(sa_profile,&sa_word) ||
            !decode_vcomp_f32_semantic(sa_word,&sa_decoded) ||
            sa_decoded.src.bank!=RegisterBank::SecondaryAttribute || sa_decoded.dst.bank!=RegisterBank::Temp)
            failures += fail("general F32 VCOMP SA/TEMP bank field roundtrip mismatch");
    }
    const uint64_t div_combine_words[]={
        0x10a4008600040f7cULL,0x10a4418600040f7cULL,
        0x10a4438600040f7cULL,0x10a4478600040f7cULL,
    };
    for (uint8_t components=1;components<=4;++components) {
        V16NmadDivF32Semantic div_combine{components};
        uint64_t word=0;
        V16NmadDivF32Semantic decoded{};
        if (!encode_v16nmad_div_f32_semantic(div_combine,&word) ||
            word!=div_combine_words[components-1] ||
            !decode_v16nmad_div_f32_semantic(word,&decoded) || decoded.components!=components)
            failures += fail("oracle F32 division V16NMAD combine mismatch");
    }
    {
        uint64_t word=0;
        V16NmadMulPackF32Semantic mul_pack{};
        V16NmadMulPackF32Semantic decoded{};
        if (!encode_v16nmad_mul_pack_f32_semantic(mul_pack,&word) ||
            word!=0x10a4478600040f7cULL ||
            !decode_v16nmad_mul_pack_f32_semantic(word,&decoded))
            failures += fail("SDK 3.0 texture-tint V16NMAD multiply-pack mismatch");
    }
    const uint64_t dot_combine_words[]={0x10c0418a00047f7cULL,0x10c0f38600047f3dULL};
    for (uint8_t components=2;components<=3;++components) {
        V16NmadDotSplatF32Semantic dot{components};
        uint64_t word=0;
        V16NmadDotSplatF32Semantic decoded{};
        if (!encode_v16nmad_dot_splat_f32_semantic(dot,&word) ||
            word!=dot_combine_words[components-2] ||
            !decode_v16nmad_dot_splat_f32_semantic(word,&decoded) || decoded.components!=components)
            failures += fail("oracle narrow dot V16NMAD reduction mismatch");
    }
    const uint64_t f32_to_s32_words[]={0x10a40084a0042000ULL,0x10a400a620041000ULL};
    for (uint8_t phase=0;phase<2;++phase) {
        V16NmadF32ToS32Semantic convert{phase};
        uint64_t word=0;
        V16NmadF32ToS32Semantic decoded{};
        if (!encode_v16nmad_f32_to_s32_semantic(convert,&word) || word!=f32_to_s32_words[phase] ||
            !decode_v16nmad_f32_to_s32_semantic(word,&decoded) || decoded.phase!=phase)
            failures += fail("oracle F32->S32 V16NMAD phase mismatch");
    }
    const uint64_t s32x2_pack_words[]={0x408106caa0000080ULL,0x4085094ea0010000ULL};
    for (uint8_t phase=0;phase<2;++phase) {
        VpckS32x2ColorSemantic pack{phase};
        uint64_t word=0;
        VpckS32x2ColorSemantic decoded{};
        if (!encode_vpck_s32x2_color_semantic(pack,&word) || word!=s32x2_pack_words[phase] ||
            !decode_vpck_s32x2_color_semantic(word,&decoded) || decoded.phase!=phase)
            failures += fail("oracle S32x2 COLOR VPCK phase mismatch");
    }
    const uint64_t s32_to_f32_pack_words[]={0x40810786a0c00081ULL,0x40810786a0800080ULL};
    for (uint8_t phase=0;phase<2;++phase) {
        VpckS32ToF32Semantic pack{phase};
        uint64_t word=0;
        VpckS32ToF32Semantic decoded{};
        if (!encode_vpck_s32_to_f32_semantic(pack,&word) || word!=s32_to_f32_pack_words[phase] ||
            !decode_vpck_s32_to_f32_semantic(word,&decoded) || decoded.phase!=phase)
            failures += fail("oracle S32->F32 VPCK phase mismatch");
    }
    {
        uint64_t word=0;
        Vmad2S32ToF32Semantic decoded{};
        if (!encode_vmad2_s32_to_f32_semantic({},&word) || word!=0x00800086a0403042ULL ||
            !decode_vmad2_s32_to_f32_semantic(word,&decoded))
            failures += fail("oracle S32->F32 VMAD2 core mismatch");
    }
    {
        Vmad2F32ScalarMadSemantic fog{};
        fog.dst={RegisterBank::Temp,60}; fog.src0={RegisterBank::Temp,60};
        fog.src1={RegisterBank::SecondaryAttribute,1}; fog.src2={RegisterBank::SecondaryAttribute,2};
        fog.src1_negative=true; fog.no_schedule=true;
        uint64_t word=0;
        Vmad2F32ScalarMadSemantic decoded{};
        if (!encode_vmad2_f32_scalar_mad_semantic(fog,&word) || word!=0x008008a0ff13c042ULL ||
            !decode_vmad2_f32_scalar_mad_semantic(word,&decoded) ||
            decoded.dst.bank!=RegisterBank::Temp || decoded.dst.num!=60 ||
            decoded.src1.bank!=RegisterBank::SecondaryAttribute || decoded.src1.num!=1 ||
            !decoded.src1_negative || !decoded.no_schedule)
            failures += fail("SDK 3.0 F32 fog VMAD2 mismatch");
    }

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

    // The standalone mvp*p oracle profile proves VMAD bit 53 is not fixed:
    // libvita2d uses 1, while this repeated external-mode VMAD uses 0.
    VmadSemantic uniform_mad{};
    uniform_mad.dst={RegisterBank::Output,0};
    uniform_mad.src1={RegisterBank::SecondaryAttribute,0};
    uniform_mad.gpi0=0; uniform_mad.gpi1=2; uniform_mad.write_mask=1;
    uniform_mad.gpi0_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}};
    uniform_mad.src1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::X,SwizzleChannel::Y}};
    uniform_mad.gpi1_swizzle={{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::Z}};
    uniform_mad.vec4=true; uniform_mad.control_bit_53=false;
    uniform_mad.repeat_mode=RepeatMode::External; uniform_mad.repeat_count=3;
    uint64_t uniform_mad_word=0;
    if (!encode_vmad_semantic(uniform_mad,&uniform_mad_word) || uniform_mad_word!=0x18903081c011a200ULL)
        failures += fail("oracle repeated VMAD semantic builder mismatch");
    VmadSemantic uniform_mad_dec{};
    if (!decode_vmad_semantic(uniform_mad_word,&uniform_mad_dec) || uniform_mad_dec.control_bit_53 ||
        uniform_mad_dec.repeat_mode!=RepeatMode::External || uniform_mad_dec.repeat_count!=3)
        failures += fail("oracle repeated VMAD semantic decode mismatch");

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

    const uint64_t f32_cmp_words[] = {
        0x48088181a0038002ULL, // ==
        0x48088281a0038002ULL, // !=
        0x48088681a0038002ULL, // <
        0x48088501a0038002ULL, // <=
        0x48088a81a0038002ULL, // >
        0x48088901a0038002ULL, // >=
    };
    const CompareOp f32_cmp_ops[] = {
        CompareOp::Equal, CompareOp::NotEqual, CompareOp::Less,
        CompareOp::LessEqual, CompareOp::Greater, CompareOp::GreaterEqual,
    };
    for (size_t i=0;i<6;i++) {
        VtstF32Semantic fcmp{};
        fcmp.lhs={RegisterBank::PrimaryAttribute,0};
        fcmp.rhs={RegisterBank::PrimaryAttribute,2};
        fcmp.op=f32_cmp_ops[i];
        uint64_t word=0;
        if (!encode_vtst_f32_semantic(fcmp,&word) || word!=f32_cmp_words[i])
            failures += fail("oracle F32 VTST semantic encode mismatch");
        VtstF32Semantic decoded{};
        if (!decode_vtst_f32_semantic(f32_cmp_words[i],&decoded) || decoded.op!=f32_cmp_ops[i] ||
            decoded.lhs.bank!=RegisterBank::PrimaryAttribute || decoded.lhs.num!=0 ||
            decoded.rhs.bank!=RegisterBank::PrimaryAttribute || decoded.rhs.num!=2)
            failures += fail("oracle F32 VTST semantic decode mismatch");
    }
    {
        VtstF32Semantic fcmp{};
        fcmp.lhs={RegisterBank::SecondaryAttribute,0};
        fcmp.rhs={RegisterBank::Special,12};
        fcmp.op=CompareOp::Greater;
        fcmp.skip_invalid=true;
        uint64_t word=0;
        VtstF32Semantic decoded{};
        if (!encode_vtst_f32_semantic(fcmp,&word) || word!=0x48898a81d003800cULL ||
            !decode_vtst_f32_semantic(word,&decoded) || decoded.op!=CompareOp::Greater ||
            decoded.lhs.bank!=RegisterBank::SecondaryAttribute || decoded.lhs.num!=0 ||
            decoded.rhs.bank!=RegisterBank::Special || decoded.rhs.num!=12)
            failures += fail("oracle F32 uniform > 0.5 VTST mismatch");
    }
    {
        VcompF32Semantic rsqrt{};
        rsqrt.op=ComplexOp::Rsqrt;
        rsqrt.dst={RegisterBank::PrimaryAttribute,0};
        rsqrt.src={RegisterBank::PrimaryAttribute,0};
        rsqrt.dest_mask=1;
        rsqrt.no_schedule=true;
        uint64_t word=0;
        VcompF32Semantic decoded{};
        if (!encode_vcomp_f32_semantic(rsqrt,&word) || word!=0x30800a0280000001ULL ||
            !decode_vcomp_f32_semantic(word,&decoded) || decoded.op!=ComplexOp::Rsqrt ||
            decoded.dst.bank!=RegisterBank::PrimaryAttribute || decoded.dst.num!=0 ||
            decoded.src.bank!=RegisterBank::PrimaryAttribute || decoded.src.num!=0)
            failures += fail("oracle F32 RSQ VCOMP mismatch");
    }
    {
        VcompF32Semantic exp2{};
        exp2.op=ComplexOp::Exp2;
        exp2.dst={RegisterBank::PrimaryAttribute,0};
        exp2.src={RegisterBank::PrimaryAttribute,0};
        uint64_t word=0;
        VcompF32Semantic decoded{};
        if (!encode_vcomp_f32_semantic(exp2,&word) || word!=0x30a0060280000001ULL ||
            !decode_vcomp_f32_semantic(word,&decoded) || decoded.op!=ComplexOp::Exp2 ||
            decoded.dst.bank!=RegisterBank::PrimaryAttribute || decoded.dst.num!=0 ||
            decoded.src.bank!=RegisterBank::PrimaryAttribute || decoded.src.num!=0)
            failures += fail("oracle Exp2 VCOMP semantic mismatch");
    }
    {
        const PackFormat src_formats[]={PackFormat::U16,PackFormat::S16};
        const uint64_t expected[]={0x40810786a0000000ULL,0x40810986a0000000ULL};
        for (size_t i=0;i<2;++i) {
            VpckSemantic pack{};
            pack.dst={RegisterBank::PrimaryAttribute,0};
            pack.src1={RegisterBank::PrimaryAttribute,0};
            pack.src2={RegisterBank::Immediate,0};
            pack.src_format=src_formats[i];
            pack.dst_format=PackFormat::F32;
            pack.dest_mask=1;
            pack.no_schedule=false;
            uint64_t word=0;
            VpckSemantic decoded{};
            if (!encode_vpck_semantic(pack,&word) || word!=expected[i] ||
                !decode_vpck_semantic(word,&decoded) || decoded.src_format!=src_formats[i] ||
                decoded.dst_format!=PackFormat::F32 || decoded.dest_mask!=1)
                failures += fail(i?"oracle S16->F32 VPCK mismatch":"oracle U16->F32 VPCK mismatch");
        }
    }
    {
        VtstF32LaneLessScalarSemantic alpha_cmp{};
        alpha_cmp.vector_lane={RegisterBank::PrimaryAttribute,1};
        alpha_cmp.scalar={RegisterBank::SecondaryAttribute,0};
        alpha_cmp.predicate_destination=1;
        alpha_cmp.lane=1;
        uint64_t word=0;
        VtstF32LaneLessScalarSemantic decoded{};
        if (!encode_vtst_f32_lane_less_scalar_semantic(alpha_cmp,&word) ||
            word!=0x4888c915b0038080ULL ||
            !decode_vtst_f32_lane_less_scalar_semantic(word,&decoded) ||
            decoded.vector_lane.bank!=RegisterBank::PrimaryAttribute || decoded.vector_lane.num!=1 ||
            decoded.scalar.bank!=RegisterBank::SecondaryAttribute || decoded.scalar.num!=0 ||
            decoded.predicate_destination!=1 || decoded.lane!=1)
            failures += fail("oracle F32 lane < scalar VTST mismatch");
    }

    VtstS32Semantic scmp{};
    scmp.lhs={RegisterBank::Temp,0};
    scmp.rhs={RegisterBank::SecondaryAttribute,0};
    scmp.op=CompareOp::Less;
    uint64_t scmp_word=0;
    if (!encode_vtst_s32_semantic(scmp,&scmp_word) || scmp_word!=0x48a8068130078000ULL)
        failures += fail("oracle S32 loop VTST semantic encode mismatch");
    VtstS32Semantic scmp_dec{};
    if (!decode_vtst_s32_semantic(scmp_word,&scmp_dec) || scmp_dec.op!=CompareOp::Less ||
        scmp_dec.lhs.bank!=RegisterBank::Temp || scmp_dec.lhs.num!=0 ||
        scmp_dec.rhs.bank!=RegisterBank::SecondaryAttribute || scmp_dec.rhs.num!=0)
        failures += fail("oracle S32 loop VTST semantic decode mismatch");

    I32Mad2Semantic loop_update{};
    loop_update.dst={RegisterBank::Temp,1};
    loop_update.src0={RegisterBank::SecondaryAttribute,3};
    loop_update.src1={RegisterBank::Temp,0};
    loop_update.src2={RegisterBank::Immediate,1};
    loop_update.sn=0;
    uint64_t imad_word=0;
    if (!encode_i32mad2_semantic(loop_update,&imad_word) || imad_word!=i32mad2_words[0])
        failures += fail("semantic loop I32MAD2 update mismatch");
    I32Mad2Semantic loop_dec{};
    if (!decode_i32mad2_semantic(imad_word,&loop_dec) || loop_dec.sn!=0 ||
        loop_dec.dst.bank!=RegisterBank::Temp || loop_dec.dst.num!=1 ||
        loop_dec.src0.bank!=RegisterBank::SecondaryAttribute || loop_dec.src0.num!=3 ||
        loop_dec.src2.bank!=RegisterBank::Immediate || loop_dec.src2.num!=1)
        failures += fail("semantic loop I32MAD2 update decode mismatch");
    loop_update.dst={RegisterBank::Temp,0};
    loop_update.src2={RegisterBank::Temp,1};
    loop_update.sn=1;
    if (!encode_i32mad2_semantic(loop_update,&imad_word) || imad_word!=i32mad2_words[1])
        failures += fail("semantic loop I32MAD2 feed-through mismatch");

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

    VbwSemantic bw_end{};
    bw_end.op=BitwiseOp::Or;
    bw_end.dst={RegisterBank::PrimaryAttribute,0};
    bw_end.src1={RegisterBank::PrimaryAttribute,0};
    bw_end.src2={RegisterBank::PrimaryAttribute,2};
    bw_end.end=true;
    if (!encode_vbw_semantic(bw_end,&bw_word) || bw_word!=0x5084000aa0000002ULL ||
        !decode_vbw_semantic(bw_word,&bw_dec) || !bw_dec.end)
        failures += fail("semantic VBW END form mismatch");

    // Scalar S32 uniform profiles isolate VBW from attribute conversion. These
    // words are emitted by Sony 1.6.5 for the clean integer oracle probes.
    struct S32BitwiseCase {
        BitwiseOp op;
        uint64_t word;
        RegisterRef src1;
        RegisterRef src2;
        bool immediate;
        uint32_t imm;
    };
    const S32BitwiseCase s32_cases[] = {
        {BitwiseOp::Or,                  0x5081000ae0000000ULL,{RegisterBank::SecondaryAttribute,0},{},true,0},
        {BitwiseOp::Or,                  0x5080000aa0000080ULL,{RegisterBank::PrimaryAttribute,1},{RegisterBank::PrimaryAttribute,0},false,0},
        {BitwiseOp::Xor,                 0x58810002a0090034ULL,{RegisterBank::PrimaryAttribute,0},{},true,4660},
        {BitwiseOp::And,                 0x50810002a000407fULL,{RegisterBank::PrimaryAttribute,0},{},true,255},
        {BitwiseOp::ShiftLeft,           0x60810002a0000003ULL,{RegisterBank::PrimaryAttribute,0},{},true,3},
        {BitwiseOp::ArithmeticShiftRight,0x6881000aa0000003ULL,{RegisterBank::PrimaryAttribute,0},{},true,3},
    };
    for (const auto &c:s32_cases) {
        VbwSemantic op{};
        op.op=c.op;
        op.dst={RegisterBank::PrimaryAttribute,0};
        op.src1=c.src1;
        op.src2=c.src2;
        op.src2_is_immediate=c.immediate;
        op.immediate=c.imm;
        uint64_t word=0;
        VbwSemantic decoded{};
        if (!encode_vbw_semantic(op,&word) || word!=c.word ||
            !decode_vbw_semantic(c.word,&decoded) || decoded.op!=c.op ||
            decoded.dst.bank!=RegisterBank::PrimaryAttribute || decoded.dst.num!=0 ||
            decoded.src1.bank!=c.src1.bank || decoded.src1.num!=c.src1.num ||
            decoded.src2_is_immediate!=c.immediate ||
            (c.immediate ? decoded.immediate!=c.imm :
                (decoded.src2.bank!=c.src2.bank || decoded.src2.num!=c.src2.num)))
            failures += fail("oracle scalar S32 VBW semantic mismatch");
    }

    VpckSemantic s32_color{};
    s32_color.dst={RegisterBank::PrimaryAttribute,0};
    s32_color.src1={RegisterBank::PrimaryAttribute,0};
    s32_color.src2={RegisterBank::Immediate,0};
    s32_color.src_format=PackFormat::S16;
    s32_color.dst_format=PackFormat::F16;
    s32_color.dest_mask=1;
    s32_color.end=true;
    s32_color.no_schedule=false;
    uint64_t s32_color_word=0;
    VpckSemantic s32_color_dec{};
    if (!encode_vpck_semantic(s32_color,&s32_color_word) || s32_color_word!=0x40850946a0000000ULL ||
        !decode_vpck_semantic(s32_color_word,&s32_color_dec) ||
        s32_color_dec.src_format!=PackFormat::S16 || s32_color_dec.dst_format!=PackFormat::F16 ||
        s32_color_dec.dst.bank!=RegisterBank::PrimaryAttribute || s32_color_dec.dst.num!=0 ||
        s32_color_dec.src1.bank!=RegisterBank::PrimaryAttribute ||
        s32_color_dec.src2.bank!=RegisterBank::Immediate || !s32_color_dec.end)
        failures += fail("oracle scalar S32 COLOR VPCK mismatch");

    KillSemantic kill{};
    kill.predicate=Predicate::P1;
    uint64_t kill_word=0;
    if (!encode_kill_semantic(kill,&kill_word) || kill_word!=0xf9300406f0000000ULL)
        failures += fail("semantic KILL canonical encoding mismatch");
    KillSemantic kill_dec{};
    if (!decode_kill_semantic(kill_words[0],&kill_dec) || kill_dec.predicate!=Predicate::P1)
        failures += fail("semantic KILL decode mismatch");
    KillSemantic kill30{};
    kill30.predicate=Predicate::P1;
    kill30.control_payload=0x306;
    uint64_t kill30_word=0;
    KillSemantic kill30_dec{};
    if (!encode_kill_semantic(kill30,&kill30_word) || kill30_word!=0xf9300406f0000306ULL ||
        !decode_kill_semantic(kill30_word,&kill30_dec) || kill30_dec.predicate!=Predicate::P1 ||
        kill30_dec.control_payload!=0x306)
        failures += fail("SDK 3.0 KILL control payload mismatch");
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
    PhaseSemantic control_phase{PhaseMode::Control};
    PhaseSemantic main_phase{PhaseMode::Main};
    PhaseSemantic decoded_phase{};
    if (!encode_phase_semantic(control_phase,&encoded) || encoded!=0xfa44010000000000ULL ||
        !decode_phase_semantic(encoded,&decoded_phase) || decoded_phase.mode!=PhaseMode::Control ||
        !encode_phase_semantic(main_phase,&encoded) || encoded!=0xfa44070000000000ULL ||
        !decode_phase_semantic(encoded,&decoded_phase) || decoded_phase.mode!=PhaseMode::Main)
        failures += fail("oracle PHAS semantic modes mismatch");
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
