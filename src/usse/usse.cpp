#include "usse/usse.hpp"
#include "usse/raw_encodings.hpp"

namespace vsc::usse {

uint8_t major_opcode(uint64_t word) { return detail::extract<uint8_t>(word, 59, 5); }

MajorClass classify_major(uint64_t word) {
    switch (major_opcode(word)) {
    case 0x00: return MajorClass::Vmad2;
    case 0x01: return MajorClass::V32Nmad;
    case 0x02: return MajorClass::V16Nmad;
    case 0x03: return MajorClass::VectorMadDot;
    case 0x04:
    case 0x05: return MajorClass::Vdual;
    case 0x06: return MajorClass::Vcomp;
    case 0x07: return MajorClass::Vmov;
    case 0x08: return MajorClass::Vpck;
    case 0x09: return MajorClass::Vtst;
    case 0x0a:
    case 0x0b:
    case 0x0c:
    case 0x0d:
    case 0x0e: return MajorClass::Vbw;
    case 0x0f: return MajorClass::VtstMask;
    case 0x1a: return MajorClass::I32Mad2;
    case 0x1c: return MajorClass::Sample;
    case 0x1f: return MajorClass::Control;
    default: return MajorClass::Unknown;
    }
}

const char *major_class_name(MajorClass cls) {
    switch (cls) {
    case MajorClass::Vmad2: return "VMAD2";
    case MajorClass::V32Nmad: return "V32NMAD";
    case MajorClass::V16Nmad: return "V16NMAD";
    case MajorClass::VectorMadDot: return "VMAD/VDP";
    case MajorClass::Vdual: return "VDUAL";
    case MajorClass::Vcomp: return "VCOMP";
    case MajorClass::Vmov: return "VMOV";
    case MajorClass::Vpck: return "VPCK";
    case MajorClass::Vtst: return "VTST";
    case MajorClass::Vbw: return "VBW";
    case MajorClass::VtstMask: return "VTSTMSK";
    case MajorClass::I32Mad2: return "I32MAD2";
    case MajorClass::Sample: return "SMP";
    case MajorClass::Control: return "CONTROL";
    case MajorClass::Unknown: return "UNKNOWN";
    }
    return "UNKNOWN";
}

ControlClass classify_control(uint64_t word) {
    if (classify_major(word) != MajorClass::Control) return ControlClass::NotControl;
    const uint8_t op2 = detail::extract<uint8_t>(word, 56, 3);
    const uint8_t opcat = detail::extract<uint8_t>(word, 52, 2);
    if (op2 == 0x2 && detail::extract<uint8_t>(word, 52, 3) == 0x4) return ControlClass::Phase;
    if (detail::extract<uint8_t>(word, 54, 1) == 0 && opcat == 0 && detail::extract<uint8_t>(word, 38, 3) == 0x5)
        return ControlClass::Nop;
    if (op2 == 0x3 && opcat == 0x2) return ControlClass::Emit;
    if (op2 == 0x4 && opcat == 0x2) return ControlClass::LoadImmediate;
    if (op2 == 0x1 && opcat == 0x3 && detail::extract<uint16_t>(word,43,9) == 0 &&
        detail::extract<uint16_t>(word,28,13) == 0x06f) return ControlClass::Kill;
    if (detail::extract<uint8_t>(word, 54, 1) == 0 && opcat == 0) return ControlClass::Branch;
    return ControlClass::Other;
}

const char *control_class_name(ControlClass cls) {
    switch (cls) {
    case ControlClass::Phase: return "PHAS";
    case ControlClass::Nop: return "NOP";
    case ControlClass::Emit: return "EMIT";
    case ControlClass::LoadImmediate: return "LIMM";
    case ControlClass::Branch: return "BR";
    case ControlClass::Kill: return "KILL";
    case ControlClass::Other: return "CONTROL-OTHER";
    case ControlClass::NotControl: return "NOT-CONTROL";
    }
    return "CONTROL-OTHER";
}

#define VSC_RAW_CODEC(name, encoding, fields_type) \
    bool decode_##name(uint64_t word, fields_type *fields) { return detail::encoding::decode(word, fields); } \
    bool encode_##name(const fields_type &fields, uint64_t *word) { return detail::encoding::encode(fields, word); }

VSC_RAW_CODEC(vmov, VmovEncoding, VmovFields)
VSC_RAW_CODEC(vpck, VpckEncoding, VpckFields)
VSC_RAW_CODEC(v32nmad, V32NmadEncoding, V32NmadFields)
VSC_RAW_CODEC(vmad, VmadEncoding, VmadFields)
VSC_RAW_CODEC(vtst, VtstEncoding, VtstFields)
VSC_RAW_CODEC(kill, KillEncoding, KillFields)
VSC_RAW_CODEC(branch, BranchEncoding, BranchFields)
VSC_RAW_CODEC(i32mad2, I32Mad2Encoding, I32Mad2Fields)

#undef VSC_RAW_CODEC

bool decode_vbw(uint64_t word, VbwFields *fields) {
    if (!detail::VbwEncoding::decode(word, fields)) return false;
    return fields->op1 >= 2 && fields->op1 <= 6;
}

bool encode_vbw(const VbwFields &fields, uint64_t *word) {
    return fields.op1 >= 2 && fields.op1 <= 6 && detail::VbwEncoding::encode(fields, word);
}

bool encode_dest_bank(RegisterBank bank, uint8_t *selector, bool *extended) {
    if (!selector || !extended) return false;
    switch (bank) {
    case RegisterBank::Temp: *selector=0; *extended=false; return true;
    case RegisterBank::Output: *selector=1; *extended=false; return true;
    case RegisterBank::PrimaryAttribute: *selector=2; *extended=false; return true;
    case RegisterBank::Indexed1: *selector=3; *extended=false; return true;
    case RegisterBank::SecondaryAttribute: *selector=0; *extended=true; return true;
    case RegisterBank::Special: *selector=1; *extended=true; return true;
    case RegisterBank::Index: *selector=2; *extended=true; return true;
    case RegisterBank::Indexed2: *selector=3; *extended=true; return true;
    default: return false;
    }
}

bool decode_dest_bank(uint8_t selector, bool extended, RegisterBank *bank) {
    if (!bank || selector > 3) return false;
    static constexpr RegisterBank base[4] = {
        RegisterBank::Temp, RegisterBank::Output,
        RegisterBank::PrimaryAttribute, RegisterBank::Indexed1
    };
    static constexpr RegisterBank ext[4] = {
        RegisterBank::SecondaryAttribute, RegisterBank::Special,
        RegisterBank::Index, RegisterBank::Indexed2
    };
    *bank = extended ? ext[selector] : base[selector];
    return true;
}

bool encode_src1_bank(RegisterBank bank, uint8_t *selector, bool *extended) {
    if (!selector || !extended) return false;
    switch (bank) {
    case RegisterBank::Temp: *selector=0; *extended=false; return true;
    case RegisterBank::Output: *selector=1; *extended=false; return true;
    case RegisterBank::PrimaryAttribute: *selector=2; *extended=false; return true;
    case RegisterBank::SecondaryAttribute: *selector=3; *extended=false; return true;
    case RegisterBank::Indexed1: *selector=0; *extended=true; return true;
    case RegisterBank::Special: *selector=1; *extended=true; return true;
    case RegisterBank::Immediate: *selector=2; *extended=true; return true;
    case RegisterBank::Indexed2: *selector=3; *extended=true; return true;
    default: return false;
    }
}

bool decode_src1_bank(uint8_t selector, bool extended, RegisterBank *bank) {
    if (!bank || selector > 3) return false;
    static constexpr RegisterBank base[4] = {
        RegisterBank::Temp, RegisterBank::Output,
        RegisterBank::PrimaryAttribute, RegisterBank::SecondaryAttribute
    };
    static constexpr RegisterBank ext[4] = {
        RegisterBank::Indexed1, RegisterBank::Special,
        RegisterBank::Immediate, RegisterBank::Indexed2
    };
    *bank = extended ? ext[selector] : base[selector];
    return true;
}

namespace {
bool encode_src0_bank(RegisterBank bank, uint8_t *selector, bool *extended) {
    if (!selector || !extended) return false;
    switch (bank) {
    case RegisterBank::Temp: *selector=0; *extended=false; return true;
    case RegisterBank::PrimaryAttribute: *selector=1; *extended=false; return true;
    case RegisterBank::Output: *selector=0; *extended=true; return true;
    case RegisterBank::SecondaryAttribute: *selector=1; *extended=true; return true;
    default: return false;
    }
}

bool decode_src0_bank(uint8_t selector, bool extended, RegisterBank *bank) {
    if (!bank || selector > 1) return false;
    if (extended)
        *bank = selector ? RegisterBank::SecondaryAttribute : RegisterBank::Output;
    else
        *bank = selector ? RegisterBank::PrimaryAttribute : RegisterBank::Temp;
    return true;
}
} // namespace

bool encode_vmov_semantic(const VmovSemantic &i, uint64_t *word) {
    if (!word || i.dst.num >= 64 || i.src.num >= 64 || i.dest_mask >= 16 ||
        i.swizzle >= 16 || i.repeat_count >= 4) return false;
    VmovFields f{};
    if (!encode_dest_bank(i.dst.bank, &f.dest_bank, &f.dest_bank_ext) ||
        !encode_src1_bank(i.src.bank, &f.src1_bank, &f.src1_bank_ext)) return false;
    f.pred = static_cast<uint8_t>(i.predicate);
    f.skip_invalid = i.skip_invalid;
    f.move_type = 0; // unconditional
    f.repeat_count = i.repeat_count;
    f.no_schedule = i.no_schedule;
    f.data_type = static_cast<uint8_t>(i.data_type);
    f.src0_swizzle = i.swizzle;
    f.dest_mask = i.dest_mask;
    f.dest_num = i.dst.num;
    f.src1_num = i.src.num;
    return encode_vmov(f, word);
}

bool decode_vmov_semantic(uint64_t word, VmovSemantic *i) {
    if (!i) return false;
    VmovFields f{};
    if (!decode_vmov(word, &f) || f.move_type != 0 || f.data_type > 7) return false;
    if (!decode_dest_bank(f.dest_bank, f.dest_bank_ext, &i->dst.bank) ||
        !decode_src1_bank(f.src1_bank, f.src1_bank_ext, &i->src.bank)) return false;
    i->dst.num = f.dest_num;
    i->src.num = f.src1_num;
    i->predicate = static_cast<Predicate>(f.pred);
    i->data_type = static_cast<DataType>(f.data_type);
    i->dest_mask = f.dest_mask;
    i->swizzle = f.src0_swizzle;
    i->repeat_count = f.repeat_count;
    i->skip_invalid = f.skip_invalid;
    i->no_schedule = f.no_schedule;
    return true;
}

bool encode_vpck_semantic(const VpckSemantic &i, uint64_t *word) {
    if (!word || i.dst.num >= 128 || i.src1.num >= 64 || i.src2.num >= 64 ||
        i.dest_mask >= 16 || i.repeat_count >= 16) return false;
    // The currently validated semantic builder covers floating-point source
    // forms. Integer pack source numbering aliases component selector bits.
    if (i.src_format != PackFormat::F16 && i.src_format != PackFormat::F32) return false;
    for (uint8_t c : i.components) if (c > 3) return false;

    VpckFields f{};
    if (!encode_dest_bank(i.dst.bank, &f.dest_bank, &f.dest_bank_ext) ||
        !encode_src1_bank(i.src1.bank, &f.src1_bank, &f.src1_bank_ext) ||
        !encode_src1_bank(i.src2.bank, &f.src2_bank, &f.src2_bank_ext)) return false;
    f.pred = static_cast<uint8_t>(i.predicate);
    f.skip_invalid = i.skip_invalid;
    f.no_schedule = i.no_schedule;
    f.end = i.end;
    f.repeat_count = i.repeat_count;
    f.src_format = static_cast<uint8_t>(i.src_format);
    f.dest_format = static_cast<uint8_t>(i.dst_format);
    f.dest_mask = i.dest_mask;
    f.dest_num = i.dst.num;
    f.component0_bit0 = (i.components[0] & 1) != 0;
    f.component0_bit1 = (i.components[0] & 2) != 0;
    f.component1 = (i.dest_mask & 0x2) ? i.components[1] : 0;
    f.component2 = (i.dest_mask & 0x4) ? i.components[2] : 0;
    f.component3 = (i.dest_mask & 0x8) ? i.components[3] : 0;
    f.scale = i.scale;
    f.src1_num = i.src1.num;
    f.src2_num = i.src2.num;
    return encode_vpck(f, word);
}

bool decode_vpck_semantic(uint64_t word, VpckSemantic *i) {
    if (!i) return false;
    VpckFields f{};
    if (!decode_vpck(word, &f)) return false;
    const auto sf = static_cast<PackFormat>(f.src_format);
    if (sf != PackFormat::F16 && sf != PackFormat::F32) return false;
    if (!decode_dest_bank(f.dest_bank, f.dest_bank_ext, &i->dst.bank) ||
        !decode_src1_bank(f.src1_bank, f.src1_bank_ext, &i->src1.bank) ||
        !decode_src1_bank(f.src2_bank, f.src2_bank_ext, &i->src2.bank)) return false;
    i->dst.num = f.dest_num;
    i->src1.num = f.src1_num;
    i->src2.num = f.src2_num;
    i->predicate = static_cast<Predicate>(f.pred);
    i->src_format = sf;
    i->dst_format = static_cast<PackFormat>(f.dest_format);
    i->dest_mask = f.dest_mask;
    i->components[0] = static_cast<uint8_t>((f.component0_bit1 ? 2 : 0) | (f.component0_bit0 ? 1 : 0));
    i->components[1] = f.component1;
    i->components[2] = f.component2;
    i->components[3] = f.component3;
    i->repeat_count = f.repeat_count;
    i->scale = f.scale;
    i->skip_invalid = f.skip_invalid;
    i->no_schedule = f.no_schedule;
    i->end = f.end;
    return true;
}


namespace {
uint16_t pack_swizzle(const Swizzle4 &s) {
    return static_cast<uint16_t>(static_cast<uint8_t>(s.c[0])) |
        (static_cast<uint16_t>(static_cast<uint8_t>(s.c[1])) << 3) |
        (static_cast<uint16_t>(static_cast<uint8_t>(s.c[2])) << 6) |
        (static_cast<uint16_t>(static_cast<uint8_t>(s.c[3])) << 9);
}
Swizzle4 unpack_swizzle(uint16_t v) {
    Swizzle4 s{};
    for (unsigned n=0;n<4;n++) s.c[n]=static_cast<SwizzleChannel>((v>>(3*n))&7);
    return s;
}
bool same_swizzle(const Swizzle4 &a, const Swizzle4 &b) {
    for (unsigned n=0;n<4;n++) if (a.c[n]!=b.c[n]) return false;
    return true;
}
// Standard vec4 table used by V32NMAD source2 and VMAD operands.
constexpr uint8_t kStdSwizzle[16][4] = {
    {0,0,0,0},{1,1,1,1},{2,2,2,2},{3,3,3,3},{0,1,2,3},{1,2,3,3},{0,1,2,2},{0,0,1,2},
    {0,1,0,1},{0,1,3,2},{2,0,1,3},{2,3,2,3},{1,2,0,2},{0,0,1,1},{0,2,3,3},{0,1,2,5}
};
bool encode_std_swizzle(const Swizzle4 &s, uint8_t *code) {
    if (!code) return false;
    for (uint8_t i=0;i<16;i++) {
        bool ok=true; for (unsigned n=0;n<4;n++) if (static_cast<uint8_t>(s.c[n])!=kStdSwizzle[i][n]) ok=false;
        if (ok) { *code=i; return true; }
    }
    return false;
}
Swizzle4 decode_std_swizzle(uint8_t code) {
    Swizzle4 s{}; code &= 15;
    for (unsigned n=0;n<4;n++) s.c[n]=static_cast<SwizzleChannel>(kStdSwizzle[code][n]);
    return s;
}
}

bool encode_v32nmad_semantic(const V32NmadSemantic &i, uint64_t *word) {
    if (!word || i.dst.num>=64 || i.src1.num>=64 || i.src2.num>=64 || i.dest_mask>=16) return false;
    V32NmadFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_bank_ext) ||
        !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_bank_ext) ||
        !encode_src1_bank(i.src2.bank,&f.src2_bank,&f.src2_bank_ext)) return false;
    const uint16_t sw1=pack_swizzle(i.src1_swizzle);
    uint8_t sw2=0; if (!encode_std_swizzle(i.src2_swizzle,&sw2)) return false;
    f.pred=static_cast<uint8_t>(i.predicate); f.skip_invalid=i.skip_invalid; f.no_schedule=i.no_schedule; f.dest_mask=i.dest_mask;
    f.dest_num=i.dst.num; f.src1_num=i.src1.num; f.src2_num=i.src2.num; f.op2=static_cast<uint8_t>(i.op);
    f.src1_swizzle_0_6=sw1&0x7f; f.src1_swizzle_7_8=(sw1>>7)&3; f.src1_swizzle_9=(sw1>>9)&1; f.src1_swizzle_10_11=(sw1>>10)&3;
    // Modifier encoding independently observed by the decoder: 0 none, 1 neg, 2 abs, 3 neg+abs.
    f.src1_mod=(i.src1_negative?1:0)|(i.src1_absolute?2:0); f.src2_mod=i.src2_absolute;
    f.src2_swizzle=sw2;
    return encode_v32nmad(f,word);
}

bool decode_v32nmad_semantic(uint64_t word, V32NmadSemantic *i) {
    if (!i) return false; V32NmadFields f{}; if (!decode_v32nmad(word,&f)) return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_bank_ext,&i->dst.bank) || !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src1.bank) || !decode_src1_bank(f.src2_bank,f.src2_bank_ext,&i->src2.bank)) return false;
    i->dst.num=f.dest_num; i->src1.num=f.src1_num; i->src2.num=f.src2_num; i->predicate=static_cast<Predicate>(f.pred); i->op=static_cast<VectorOp>(f.op2); i->dest_mask=f.dest_mask;
    uint16_t sw=f.src1_swizzle_0_6|(f.src1_swizzle_7_8<<7)|(static_cast<uint16_t>(f.src1_swizzle_9)<<9)|(static_cast<uint16_t>(f.src1_swizzle_10_11)<<10);
    i->src1_swizzle=unpack_swizzle(sw); i->src2_swizzle=decode_std_swizzle(f.src2_swizzle);
    i->src1_negative=(f.src1_mod&1)!=0; i->src1_absolute=(f.src1_mod&2)!=0; i->src2_absolute=f.src2_mod;
    i->skip_invalid=f.skip_invalid; i->no_schedule=f.no_schedule; return true;
}

bool encode_vmad_semantic(const VmadSemantic &i, uint64_t *word) {
    if (!word || i.dst.num>=64 || i.src1.num>=64 || i.gpi0>=4 || i.gpi1>=4 || i.write_mask>=16 || i.repeat_count>=4) return false;
    VmadFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_bank_ext) || !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_bank_ext)) return false;
    uint8_t g0=0,g1=0,s1=0; if (!encode_std_swizzle(i.gpi0_swizzle,&g0) || !encode_std_swizzle(i.gpi1_swizzle,&g1) || !encode_std_swizzle(i.src1_swizzle,&s1)) return false;
    f.pred=static_cast<uint8_t>(i.predicate); f.skip_invalid=i.skip_invalid; f.control_bit_53=i.control_bit_53;
    f.opcode2=i.vec4; f.repeat_mode=static_cast<uint8_t>(i.repeat_mode); f.repeat_count=i.repeat_count;
    f.no_schedule=i.no_schedule; f.write_mask=i.write_mask; f.dest_num=i.dst.num; f.src1_num=i.src1.num; f.gpi0_num=i.gpi0; f.gpi1_num=i.gpi1;
    f.gpi0_swizzle=g0; f.gpi1_swizzle=g1; f.src1_swizzle=s1;
    return encode_vmad(f,word);
}

bool decode_vmad_semantic(uint64_t word, VmadSemantic *i) {
    if (!i) return false; VmadFields f{}; if (!decode_vmad(word,&f)) return false;
    // Semantic v1 deliberately supports the non-extended, unmodified forms used by vita2d.
    if (f.gpi0_swizzle_ext || f.gpi1_swizzle_ext || f.src1_swizzle_ext || f.gpi0_abs || f.gpi0_neg || f.gpi1_abs || f.gpi1_neg || f.src1_abs || f.src1_neg) return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_bank_ext,&i->dst.bank) || !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src1.bank)) return false;
    i->dst.num=f.dest_num; i->src1.num=f.src1_num; i->predicate=static_cast<Predicate>(f.pred); i->gpi0=f.gpi0_num; i->gpi1=f.gpi1_num; i->write_mask=f.write_mask;
    i->gpi0_swizzle=decode_std_swizzle(f.gpi0_swizzle); i->gpi1_swizzle=decode_std_swizzle(f.gpi1_swizzle); i->src1_swizzle=decode_std_swizzle(f.src1_swizzle);
    i->vec4=f.opcode2; i->control_bit_53=f.control_bit_53; i->repeat_mode=static_cast<RepeatMode>(f.repeat_mode);
    i->repeat_count=f.repeat_count; i->skip_invalid=f.skip_invalid; i->no_schedule=f.no_schedule;
    return true;
}

namespace {
bool encode_compare_test(CompareOp op, uint8_t *zero_test, uint8_t *sign_test) {
    if (!zero_test || !sign_test) return false;
    switch (op) {
    case CompareOp::Equal:        *zero_test=1; *sign_test=0; return true;
    case CompareOp::NotEqual:     *zero_test=2; *sign_test=0; return true;
    case CompareOp::Less:         *zero_test=2; *sign_test=1; return true;
    case CompareOp::LessEqual:    *zero_test=1; *sign_test=1; return true;
    case CompareOp::Greater:      *zero_test=2; *sign_test=2; return true;
    case CompareOp::GreaterEqual: *zero_test=1; *sign_test=2; return true;
    }
    return false;
}

bool decode_compare_test(uint8_t zero_test, uint8_t sign_test, CompareOp *op) {
    if (!op) return false;
    if (zero_test==1 && sign_test==0) { *op=CompareOp::Equal; return true; }
    if (zero_test==2 && sign_test==0) { *op=CompareOp::NotEqual; return true; }
    if (zero_test==2 && sign_test==1) { *op=CompareOp::Less; return true; }
    if (zero_test==1 && sign_test==1) { *op=CompareOp::LessEqual; return true; }
    if (zero_test==2 && sign_test==2) { *op=CompareOp::Greater; return true; }
    if (zero_test==1 && sign_test==2) { *op=CompareOp::GreaterEqual; return true; }
    return false;
}
} // namespace

bool encode_vtst_semantic(const VtstSemantic &i, uint64_t *word) {
    if (!word || i.lhs.num>=128 || i.rhs.num>=128 || i.predicate_destination>=4 || i.component>=4)
        return false;
    VtstFields f{};
    if (!encode_src1_bank(i.lhs.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.rhs.bank,&f.src2_bank,&f.src2_ext)) return false;

    f.pred=static_cast<uint8_t>(i.predicate);
    f.skip_invalid=i.skip_invalid;
    f.dest_ext=true;
    f.zero_test=0;
    f.sign_test=0;
    if (!encode_compare_test(i.op,&f.zero_test,&f.sign_test)) return false;
    f.test_crcomb_and=true;
    f.channel=i.component;
    f.predicate_destination=i.predicate_destination;
    // This fixed destination/ALU form is the observed U32 subtract-and-test CMP.
    f.dest_bank=1;
    f.dest_num=0;
    f.test_write_enable=false;
    f.alu_select=1;
    f.alu_op=15;
    f.src1_num=i.lhs.num;
    f.src2_num=i.rhs.num;
    return encode_vtst(f,word);
}

bool decode_vtst_semantic(uint64_t word, VtstSemantic *i) {
    if (!i) return false;
    VtstFields f{};
    if (!decode_vtst(word,&f)) return false;
    if (!f.dest_ext || f.dest_bank!=1 || f.dest_num!=0 || f.test_write_enable ||
        f.alu_select!=1 || f.alu_op!=15 || f.precision || f.src1_negative ||
        f.src2_vector_scalar_component || f.repeat_count!=0 || f.once_only ||
        f.sync_start || !f.test_crcomb_and || f.channel>=4) return false;
    if (!decode_src1_bank(f.src1_bank,f.src1_ext,&i->lhs.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->rhs.bank)) return false;
    if (!decode_compare_test(f.zero_test,f.sign_test,&i->op)) return false;
    i->lhs.num=f.src1_num;
    i->rhs.num=f.src2_num;
    i->predicate=static_cast<Predicate>(f.pred);
    i->predicate_destination=f.predicate_destination;
    i->component=f.channel;
    i->skip_invalid=f.skip_invalid;
    return true;
}

namespace {
bool encode_f32_compare_test(CompareOp op, uint8_t *zero_test, uint8_t *sign_test,
                             bool *crcomb_and) {
    if (!zero_test || !sign_test || !crcomb_and) return false;
    if (!encode_compare_test(op, zero_test, sign_test)) return false;
    *crcomb_and = op != CompareOp::LessEqual && op != CompareOp::GreaterEqual;
    return true;
}

bool decode_f32_compare_test(uint8_t zero_test, uint8_t sign_test, bool crcomb_and,
                             CompareOp *op) {
    if (!decode_compare_test(zero_test, sign_test, op)) return false;
    const bool expected = *op != CompareOp::LessEqual && *op != CompareOp::GreaterEqual;
    return crcomb_and == expected;
}
} // namespace

bool encode_vtst_f32_semantic(const VtstF32Semantic &i, uint64_t *word) {
    if (!word || i.lhs.num>=128 || i.rhs.num>=128 || i.predicate_destination>=4 || i.component>=4)
        return false;
    VtstFields f{};
    if (!encode_src1_bank(i.lhs.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.rhs.bank,&f.src2_bank,&f.src2_ext)) return false;
    f.pred=static_cast<uint8_t>(i.predicate);
    f.skip_invalid=i.skip_invalid;
    f.dest_ext=true;
    f.precision=true;
    if (!encode_f32_compare_test(i.op,&f.zero_test,&f.sign_test,&f.test_crcomb_and)) return false;
    f.channel=i.component;
    f.predicate_destination=i.predicate_destination;
    f.dest_bank=1;
    f.dest_num=0;
    f.test_write_enable=false;
    f.alu_select=0;
    f.alu_op=14;
    f.src1_num=i.lhs.num;
    f.src2_num=i.rhs.num;
    return encode_vtst(f,word);
}

bool decode_vtst_f32_semantic(uint64_t word, VtstF32Semantic *i) {
    if (!i) return false;
    VtstFields f{};
    if (!decode_vtst(word,&f)) return false;
    if (!f.dest_ext || f.dest_bank!=1 || f.dest_num!=0 || f.test_write_enable ||
        f.alu_select!=0 || f.alu_op!=14 || !f.precision || f.src1_negative ||
        f.src2_vector_scalar_component || f.repeat_count!=0 || f.once_only || f.sync_start ||
        f.channel>=4) return false;
    if (!decode_src1_bank(f.src1_bank,f.src1_ext,&i->lhs.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->rhs.bank) ||
        !decode_f32_compare_test(f.zero_test,f.sign_test,f.test_crcomb_and,&i->op)) return false;
    i->lhs.num=f.src1_num;
    i->rhs.num=f.src2_num;
    i->predicate=static_cast<Predicate>(f.pred);
    i->predicate_destination=f.predicate_destination;
    i->component=f.channel;
    i->skip_invalid=f.skip_invalid;
    return true;
}

bool encode_vtst_s32_semantic(const VtstS32Semantic &i, uint64_t *word) {
    if (!word || i.op!=CompareOp::Less || i.lhs.num>=128 || i.rhs.num>=128 ||
        i.predicate_destination>=4) return false;
    VtstFields f{};
    if (!encode_src1_bank(i.lhs.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.rhs.bank,&f.src2_bank,&f.src2_ext)) return false;
    f.pred=static_cast<uint8_t>(i.predicate);
    f.skip_invalid=i.skip_invalid;
    f.once_only=true;
    f.dest_ext=true;
    f.precision=false;
    f.zero_test=2;
    f.sign_test=1;
    f.test_crcomb_and=true;
    f.channel=0;
    f.predicate_destination=i.predicate_destination;
    f.dest_bank=1;
    f.dest_num=0;
    f.test_write_enable=false;
    f.alu_select=1;
    f.alu_op=14;
    f.src1_num=i.lhs.num;
    f.src2_num=i.rhs.num;
    return encode_vtst(f,word);
}

bool decode_vtst_s32_semantic(uint64_t word, VtstS32Semantic *i) {
    if (!i) return false;
    VtstFields f{};
    if (!decode_vtst(word,&f)) return false;
    if (!f.dest_ext || f.dest_bank!=1 || f.dest_num!=0 || f.test_write_enable ||
        f.alu_select!=1 || f.alu_op!=14 || f.precision || f.src1_negative ||
        f.src2_vector_scalar_component || f.repeat_count!=0 || !f.once_only || f.sync_start ||
        !f.test_crcomb_and || f.channel!=0 || f.zero_test!=2 || f.sign_test!=1)
        return false;
    if (!decode_src1_bank(f.src1_bank,f.src1_ext,&i->lhs.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->rhs.bank)) return false;
    i->lhs.num=f.src1_num;
    i->rhs.num=f.src2_num;
    i->predicate=static_cast<Predicate>(f.pred);
    i->predicate_destination=f.predicate_destination;
    i->op=CompareOp::Less;
    i->skip_invalid=f.skip_invalid;
    return true;
}

bool encode_i32mad2_semantic(const I32Mad2Semantic &i, uint64_t *word) {
    if (!word || i.sn>1 || i.dst.num>=128 || i.src0.num>=128 ||
        i.src1.num>=128 || i.src2.num>=128) return false;
    I32Mad2Fields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_ext) ||
        !encode_src0_bank(i.src0.bank,&f.src0_bank,&f.src0_ext) ||
        !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.src2.bank,&f.src2_bank,&f.src2_ext)) return false;
    f.pred=static_cast<uint8_t>(Predicate::Always);
    f.dontcare=true;
    f.no_schedule=false;
    f.sn=i.sn;
    f.end=false;
    f.count=0;
    f.is_signed=false;
    f.negative_src1=false;
    f.negative_src2=false;
    f.dest_num=i.dst.num;
    f.src0_num=i.src0.num;
    f.src1_num=i.src1.num;
    f.src2_num=i.src2.num;
    return encode_i32mad2(f,word);
}

bool decode_i32mad2_semantic(uint64_t word, I32Mad2Semantic *i) {
    if (!i) return false;
    I32Mad2Fields f{};
    if (!decode_i32mad2(word,&f)) return false;
    if (f.pred!=static_cast<uint8_t>(Predicate::Always) || !f.dontcare || f.no_schedule ||
        f.sn>1 || f.end || f.count!=0 || f.is_signed || f.negative_src1 || f.negative_src2)
        return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_ext,&i->dst.bank) ||
        !decode_src0_bank(f.src0_bank,f.src0_ext,&i->src0.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_ext,&i->src1.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->src2.bank)) return false;
    i->dst.num=f.dest_num;
    i->src0.num=f.src0_num;
    i->src1.num=f.src1_num;
    i->src2.num=f.src2_num;
    i->sn=f.sn;
    return true;
}

namespace {
uint32_t rotate_left32(uint32_t value, uint8_t amount) {
    amount &= 31;
    return amount ? static_cast<uint32_t>((value<<amount)|(value>>(32-amount))) : value;
}

uint32_t rotate_right32(uint32_t value, uint8_t amount) {
    amount &= 31;
    return amount ? static_cast<uint32_t>((value>>amount)|(value<<(32-amount))) : value;
}

bool encode_vbw_op(BitwiseOp op, uint8_t *op1, bool *op2) {
    if (!op1 || !op2) return false;
    switch (op) {
    case BitwiseOp::And:                  *op1=2; *op2=false; return true;
    case BitwiseOp::Or:                   *op1=2; *op2=true; return true;
    case BitwiseOp::Xor:                  *op1=3; *op2=false; return true;
    case BitwiseOp::ShiftLeft:            *op1=4; *op2=false; return true;
    case BitwiseOp::RotateLeft:           *op1=4; *op2=true; return true;
    case BitwiseOp::ShiftRight:           *op1=5; *op2=false; return true;
    case BitwiseOp::ArithmeticShiftRight: *op1=5; *op2=true; return true;
    }
    return false;
}

bool decode_vbw_op(uint8_t op1, bool op2, BitwiseOp *op) {
    if (!op) return false;
    if (op1==2) { *op=op2?BitwiseOp::Or:BitwiseOp::And; return true; }
    if (op1==3) { *op=BitwiseOp::Xor; return true; }
    if (op1==4) { *op=op2?BitwiseOp::RotateLeft:BitwiseOp::ShiftLeft; return true; }
    if (op1==5) { *op=op2?BitwiseOp::ArithmeticShiftRight:BitwiseOp::ShiftRight; return true; }
    return false;
}

bool encode_vbw_immediate(uint32_t immediate, VbwFields *f) {
    if (!f) return false;
    for (uint8_t invert=0; invert<2; ++invert) {
        const uint32_t target=invert ? ~immediate : immediate;
        for (uint8_t rotate=0; rotate<32; ++rotate) {
            const uint32_t base=rotate_right32(target,rotate);
            if (base>0xffffu) continue;
            f->src2_invert=invert!=0;
            f->src2_rotate=rotate;
            f->src2_num=static_cast<uint8_t>(base&0x7f);
            f->src2_select=static_cast<uint8_t>((base>>7)&0x7f);
            f->src2_extra_high=static_cast<uint8_t>((base>>14)&0x3);
            return true;
        }
    }
    return false;
}

uint32_t decode_vbw_immediate(const VbwFields &f) {
    uint32_t value=static_cast<uint32_t>(f.src2_num) |
        (static_cast<uint32_t>(f.src2_select)<<7) |
        (static_cast<uint32_t>(f.src2_extra_high)<<14);
    value=rotate_left32(value,f.src2_rotate);
    return f.src2_invert ? ~value : value;
}
} // namespace

bool encode_vbw_semantic(const VbwSemantic &i, uint64_t *word) {
    if (!word || i.dst.num>=128 || i.src1.num>=128 || i.repeat_count>=16 ||
        (!i.src2_is_immediate && i.src2.num>=128)) return false;
    VbwFields f{};
    if (!encode_vbw_op(i.op,&f.op1,&f.op2) ||
        !encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_ext) ||
        !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_ext)) return false;
    f.pred=static_cast<uint8_t>(i.predicate);
    f.skip_invalid=i.skip_invalid;
    f.no_schedule=i.no_schedule;
    f.repeat_count=i.repeat_count;
    f.dest_num=i.dst.num;
    f.src1_num=i.src1.num;
    if (i.src2_is_immediate) {
        if (!encode_src1_bank(RegisterBank::Immediate,&f.src2_bank,&f.src2_ext) ||
            !encode_vbw_immediate(i.immediate,&f)) return false;
    } else {
        if (!encode_src1_bank(i.src2.bank,&f.src2_bank,&f.src2_ext)) return false;
        f.src2_num=i.src2.num;
    }
    return encode_vbw(f,word);
}

bool decode_vbw_semantic(uint64_t word, VbwSemantic *i) {
    if (!i) return false;
    VbwFields f{};
    if (!decode_vbw(word,&f) || f.partial || f.repeat_select || f.sync_start || f.end)
        return false;
    if (!decode_vbw_op(f.op1,f.op2,&i->op) ||
        !decode_dest_bank(f.dest_bank,f.dest_ext,&i->dst.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_ext,&i->src1.bank)) return false;
    i->dst.num=f.dest_num;
    i->src1.num=f.src1_num;
    i->predicate=static_cast<Predicate>(f.pred);
    i->repeat_count=f.repeat_count;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
    RegisterBank src2_bank=RegisterBank::Invalid;
    if (!decode_src1_bank(f.src2_bank,f.src2_ext,&src2_bank)) return false;
    i->src2_is_immediate=src2_bank==RegisterBank::Immediate;
    if (i->src2_is_immediate) {
        i->src2={};
        i->immediate=decode_vbw_immediate(f);
    } else {
        // Rotation/inversion/extended immediate fields are meaningful only for
        // immediate source2 in the semantic subset exposed here.
        if (f.src2_rotate || f.src2_invert || f.src2_extra_high || f.src2_select) return false;
        i->src2.bank=src2_bank;
        i->src2.num=f.src2_num;
        i->immediate=0;
    }
    return true;
}

namespace {
bool encode_short_predicate(Predicate predicate, uint8_t *short_predicate) {
    if (!short_predicate) return false;
    switch (predicate) {
    case Predicate::Always: *short_predicate=0; return true;
    case Predicate::P0: *short_predicate=1; return true;
    case Predicate::P1: *short_predicate=2; return true;
    case Predicate::NotP0: *short_predicate=3; return true;
    default: return false;
    }
}

bool decode_short_predicate(uint8_t short_predicate, Predicate *predicate) {
    if (!predicate) return false;
    switch (short_predicate) {
    case 0: *predicate=Predicate::Always; return true;
    case 1: *predicate=Predicate::P0; return true;
    case 2: *predicate=Predicate::P1; return true;
    case 3: *predicate=Predicate::NotP0; return true;
    default: return false;
    }
}
} // namespace

bool encode_kill_semantic(const KillSemantic &i, uint64_t *word) {
    KillFields f{};
    if (!encode_short_predicate(i.predicate,&f.short_predicate)) return false;
    return encode_kill(f,word);
}

bool decode_kill_semantic(uint64_t word, KillSemantic *i) {
    if (!i) return false;
    KillFields f{};
    if (!decode_kill(word,&f)) return false;
    return decode_short_predicate(f.short_predicate,&i->predicate);
}

namespace {
bool validated_branch_predicate(Predicate predicate) {
    return predicate == Predicate::Always || predicate == Predicate::P0 ||
        predicate == Predicate::NotP0;
}
} // namespace

bool encode_branch_semantic(const BranchSemantic &i, uint64_t *word) {
    if (!word || !validated_branch_predicate(i.predicate) ||
        i.offset < -(1 << 19) || i.offset >= (1 << 19)) return false;
    BranchFields f{};
    f.pred = static_cast<uint8_t>(i.predicate);
    f.offset = static_cast<uint32_t>(i.offset) & 0x000fffffu;
    return encode_branch(f, word);
}

bool decode_branch_semantic(uint64_t word, BranchSemantic *i) {
    if (!i) return false;
    BranchFields f{};
    if (!decode_branch(word, &f)) return false;
    i->predicate = static_cast<Predicate>(f.pred);
    if (!validated_branch_predicate(i->predicate)) return false;
    const uint32_t raw = f.offset & 0x000fffffu;
    i->offset = (raw & 0x00080000u) ?
        static_cast<int32_t>(raw | 0xfff00000u) : static_cast<int32_t>(raw);
    return true;
}

bool encode(const Instruction &instruction, uint64_t *word) {
    if (!word) return false;
    switch (instruction.opcode) {
    case Opcode::Phase: *word=0xfa44070000000000ULL; return true;
    case Opcode::Nop: *word=0xf800094000000000ULL; return true;
    case Opcode::Emit: *word=0xfb275000a0200000ULL; return true;
    default: return false;
    }
}

} // namespace vsc::usse
