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
VSC_RAW_CODEC(vmad2, Vmad2Encoding, Vmad2Fields)
VSC_RAW_CODEC(v32nmad, V32NmadEncoding, V32NmadFields)
VSC_RAW_CODEC(vcomp, VcompEncoding, VcompFields)
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

bool encode_phase_semantic(const PhaseSemantic &i, uint64_t *word) {
    if (!word || (i.mode!=PhaseMode::Control && i.mode!=PhaseMode::Main)) return false;
    *word=0xfa44000000000000ULL |
        (static_cast<uint64_t>(static_cast<uint8_t>(i.mode))<<40);
    return true;
}

bool decode_phase_semantic(uint64_t word, PhaseSemantic *i) {
    if (!i || classify_control(word)!=ControlClass::Phase ||
        (word & ~(uint64_t{0x7}<<40))!=0xfa44000000000000ULL) return false;
    const uint8_t mode=static_cast<uint8_t>((word>>40)&0x7u);
    if (mode!=static_cast<uint8_t>(PhaseMode::Control) &&
        mode!=static_cast<uint8_t>(PhaseMode::Main)) return false;
    i->mode=static_cast<PhaseMode>(mode);
    return true;
}

bool encode_nop_semantic(const NopSemantic &i, uint64_t *word) {
    if (!word) return false;
    *word=0xf800094000000000ULL;
    if (!i.no_schedule) *word&=~(uint64_t{1}<<43);
    if (i.end) *word|=uint64_t{1}<<50;
    return true;
}

bool decode_nop_semantic(uint64_t word, NopSemantic *i) {
    if (!i || classify_control(word)!=ControlClass::Nop) return false;
    const uint64_t allowed=0xf800094000000000ULL | (uint64_t{1}<<43) | (uint64_t{1}<<50);
    if ((word & ~((uint64_t{1}<<43)|(uint64_t{1}<<50))) !=
        (0xf800094000000000ULL & ~((uint64_t{1}<<43)|(uint64_t{1}<<50))) ||
        (word & ~allowed)) return false;
    i->no_schedule=((word>>43)&1u)!=0;
    i->end=((word>>50)&1u)!=0;
    return true;
}

bool encode_vmov_semantic(const VmovSemantic &i, uint64_t *word) {
    if (!word || i.dst.num >= 64 || i.src.num >= 64 || i.dest_mask >= 16 ||
        i.swizzle >= 16 || i.repeat_count >= 4) return false;
    VmovFields f{};
    if (!encode_dest_bank(i.dst.bank, &f.dest_bank, &f.dest_bank_ext) ||
        !encode_src1_bank(i.src.bank, &f.src1_bank, &f.src1_bank_ext)) return false;
    f.pred = static_cast<uint8_t>(i.predicate);
    f.skip_invalid = i.skip_invalid;
    f.end_or_src0_bank_ext = i.end;
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
    i->end = f.end_or_src0_bank_ext;
    return true;
}

bool encode_vmovc_f32_lt_zero_semantic(const VmovcF32LtZeroSemantic &i, uint64_t *word) {
    if (!word || i.predicate!=Predicate::Always || i.dest_mask==0 || i.dest_mask>=16 ||
        i.swizzle>=16 || i.dst.num>=64 || i.test.num>=64 ||
        i.src_true.num>=64 || i.src_false.num>=64)
        return false;
    VmovFields f{};
    bool src0_ext=false;
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_bank_ext) ||
        !encode_src0_bank(i.test.bank,&f.src0_bank,&src0_ext) ||
        !encode_src1_bank(i.src_true.bank,&f.src1_bank,&f.src1_bank_ext) ||
        !encode_src1_bank(i.src_false.bank,&f.src2_bank,&f.src2_bank_ext))
        return false;
    f.pred=0;
    f.skip_invalid=i.skip_invalid;
    f.test_bit_2=true;
    f.src0_component_select=false;
    f.sync_start=false;
    f.end_or_src0_bank_ext=src0_ext;
    f.move_type=1;
    f.repeat_count=0;
    f.no_schedule=i.no_schedule;
    f.data_type=static_cast<uint8_t>(DataType::F32);
    f.test_bit_1=false;
    f.src0_swizzle=i.swizzle;
    f.dest_mask=i.dest_mask;
    f.dest_num=i.dst.num;
    f.src0_num=i.test.num;
    f.src1_num=i.src_true.num;
    f.src2_num=i.src_false.num;
    return encode_vmov(f,word);
}

bool decode_vmovc_f32_lt_zero_semantic(uint64_t word, VmovcF32LtZeroSemantic *i) {
    if (!i) return false;
    VmovFields f{};
    if (!decode_vmov(word,&f) || f.pred!=0 || f.move_type!=1 || !f.test_bit_2 || f.test_bit_1 ||
        f.src0_component_select || f.sync_start || f.repeat_count!=0 ||
        f.data_type!=static_cast<uint8_t>(DataType::F32) || f.dest_mask==0 || f.src0_swizzle>=16)
        return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_bank_ext,&i->dst.bank) ||
        !decode_src0_bank(f.src0_bank,f.end_or_src0_bank_ext,&i->test.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src_true.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_bank_ext,&i->src_false.bank))
        return false;
    i->dst.num=f.dest_num;
    i->test.num=f.src0_num;
    i->src_true.num=f.src1_num;
    i->src_false.num=f.src2_num;
    i->predicate=Predicate::Always;
    i->dest_mask=f.dest_mask;
    i->swizzle=f.src0_swizzle;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
    return true;
}

bool encode_vpck_semantic(const VpckSemantic &i, uint64_t *word) {
    if (!word || i.dst.num >= 128 || i.src1.num >= 64 || i.src2.num >= 64 ||
        i.dest_mask >= 16 || i.repeat_count >= 16) return false;
    // Integer source numbering aliases component selector bits. Expose only
    // scalar integer forms independently anchored by Sony probes: S16->F16
    // COLOR and U16/S16->F32 arithmetic conversion.
    const bool scalar_s16=i.src_format==PackFormat::S16 && i.dst_format==PackFormat::F16 &&
        i.dest_mask==1 && i.components[0]==0 && i.repeat_count==0 && !i.scale &&
        i.src2.bank==RegisterBank::Immediate && i.src2.num==0;
    const bool scalar_16_f32=(i.src_format==PackFormat::U16 || i.src_format==PackFormat::S16) &&
        i.dst_format==PackFormat::F32 && i.dest_mask==1 && i.components[0]==0 &&
        i.repeat_count==0 && !i.scale;
    if (i.src_format != PackFormat::F16 && i.src_format != PackFormat::F32 &&
        !scalar_s16 && !scalar_16_f32) return false;
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
    const auto df = static_cast<PackFormat>(f.dest_format);
    const bool scalar_s16=sf==PackFormat::S16 && df==PackFormat::F16 && f.dest_mask==1 &&
        !f.component0_bit0 && !f.component0_bit1 && f.repeat_count==0 && !f.scale &&
        f.src2_bank==2 && f.src2_bank_ext && f.src2_num==0;
    const bool scalar_16_f32=(sf==PackFormat::U16 || sf==PackFormat::S16) && df==PackFormat::F32 &&
        f.dest_mask==1 && !f.component0_bit0 && !f.component0_bit1 && f.repeat_count==0 && !f.scale;
    if (sf != PackFormat::F16 && sf != PackFormat::F32 && !scalar_s16 && !scalar_16_f32) return false;
    if (!decode_dest_bank(f.dest_bank, f.dest_bank_ext, &i->dst.bank) ||
        !decode_src1_bank(f.src1_bank, f.src1_bank_ext, &i->src1.bank) ||
        !decode_src1_bank(f.src2_bank, f.src2_bank_ext, &i->src2.bank)) return false;
    i->dst.num = f.dest_num;
    i->src1.num = f.src1_num;
    i->src2.num = f.src2_num;
    i->predicate = static_cast<Predicate>(f.pred);
    i->src_format = sf;
    i->dst_format = df;
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

bool encode_vpck16_to_f32_semantic(const Vpck16ToF32Semantic &i, uint64_t *word) {
    if (!word || (i.src_format!=PackFormat::U16 && i.src_format!=PackFormat::S16) ||
        i.component>=4 || i.repeat_count>3 || i.dst.num>=128 || i.src.num>=64)
        return false;
    VpckFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_bank_ext) ||
        !encode_src1_bank(i.src.bank,&f.src1_bank,&f.src1_bank_ext) ||
        !encode_src1_bank(RegisterBank::Immediate,&f.src2_bank,&f.src2_bank_ext))
        return false;
    f.pred=static_cast<uint8_t>(Predicate::Always);
    f.skip_invalid=i.skip_invalid;
    f.no_schedule=i.no_schedule;
    f.repeat_count=i.repeat_count;
    f.src_format=static_cast<uint8_t>(i.src_format);
    f.dest_format=static_cast<uint8_t>(PackFormat::F32);
    f.dest_mask=1;
    f.dest_num=i.dst.num;
    f.scale=i.scale;
    f.src1_num=i.src.num;
    f.src2_num=0;
    f.component0_bit0=(i.component&1u)!=0;
    f.component0_bit1=(i.component&2u)!=0;
    return encode_vpck(f,word);
}

bool decode_vpck16_to_f32_semantic(uint64_t word, Vpck16ToF32Semantic *i) {
    if (!i) return false;
    VpckFields f{};
    if (!decode_vpck(word,&f) || f.pred!=static_cast<uint8_t>(Predicate::Always) ||
        !f.skip_invalid || f.unknown || f.sync_start || f.end || f.repeat_count>3 ||
        (f.src_format!=static_cast<uint8_t>(PackFormat::U16) &&
         f.src_format!=static_cast<uint8_t>(PackFormat::S16)) ||
        f.dest_format!=static_cast<uint8_t>(PackFormat::F32) || f.dest_mask!=1 ||
        f.component1 || f.component2 || f.component3 || f.src2_num!=0)
        return false;
    RegisterBank src2{};
    if (!decode_dest_bank(f.dest_bank,f.dest_bank_ext,&i->dst.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_bank_ext,&src2) || src2!=RegisterBank::Immediate)
        return false;
    i->dst.num=f.dest_num;
    i->src.num=f.src1_num;
    i->src_format=static_cast<PackFormat>(f.src_format);
    i->component=static_cast<uint8_t>((f.component0_bit1?2u:0u)|(f.component0_bit0?1u:0u));
    i->repeat_count=f.repeat_count;
    i->scale=f.scale;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
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
constexpr Swizzle4 kVmad3Src1XY0{{SwizzleChannel::X,SwizzleChannel::Y,
                                 SwizzleChannel::Zero,SwizzleChannel::X}};
const Swizzle4 kVmad2Src0Swizzles[8]={
    {{SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X}},
    {{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y}},
    {{SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z}},
    {{SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}},
    {{SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::X,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::Z,SwizzleChannel::W,SwizzleChannel::X,SwizzleChannel::Y}},
};
const Swizzle4 kVmad2Src1Swizzles[8]={
    {{SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X}},
    {{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y}},
    {{SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z}},
    {{SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Z}},
    {{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::W,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}},
};
const Swizzle4 kVmad2Src2Swizzles[8]={
    {{SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::X}},
    {{SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y,SwizzleChannel::Y}},
    {{SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z,SwizzleChannel::Z}},
    {{SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::Z,SwizzleChannel::W,SwizzleChannel::W}},
    {{SwizzleChannel::X,SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z}},
    {{SwizzleChannel::X,SwizzleChannel::Y,SwizzleChannel::Z,SwizzleChannel::Z}},
};
bool encode_vmad2_swizzle(const Swizzle4 &s,const Swizzle4 *table,uint8_t *code) {
    if (!code) return false;
    for (uint8_t i=0;i<8;++i) if (same_swizzle(s,table[i])) { *code=i; return true; }
    return false;
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

bool encode_vcomp_rcp_f32(const VcompRcpF32Fields &f, uint64_t *word) {
    if (!word || f.component>=4 || (f.source_odd && f.component>=2) ||
        (f.scalar && (f.source_odd || f.component>=2))) return false;
    static constexpr uint8_t kLaneLow[4]={0x01,0x02,0x84,0x88};
    uint64_t encoded=0x308008008f800000ULL | (static_cast<uint64_t>(f.source_pair)<<8);
    if (f.scalar) {
        encoded|=0x01u;
        if (f.component==1) encoded|=uint64_t{1}<<35;
    } else {
        encoded|=kLaneLow[f.component];
        if (f.source_odd) encoded|=0x80u;
        if (f.component&1u) encoded|=uint64_t{1}<<35;
    }
    *word=encoded;
    return true;
}

bool decode_vcomp_rcp_f32(uint64_t word, VcompRcpF32Fields *f) {
    if (!f) return false;
    constexpr uint64_t variable=(uint64_t{1}<<35)|(uint64_t{0xff}<<8)|uint64_t{0xff};
    constexpr uint64_t base=0x308008008f800000ULL;
    if ((word&~variable)!=(base&~variable)) return false;
    const bool odd=((word>>35)&1u)!=0;
    const uint8_t low=static_cast<uint8_t>(word);
    uint8_t component=0xff;
    bool source_odd=false;
    bool scalar=false;
    if (!odd && low==0x01) { component=0; scalar=true; }
    else if (odd && low==0x01) { component=1; scalar=true; }
    else if (odd && low==0x02) component=1;
    else if (!odd && low==0x81) { component=0; source_odd=true; }
    else if (odd && low==0x82) { component=1; source_odd=true; }
    else if (!odd && low==0x84) component=2;
    else if (odd && low==0x88) component=3;
    if (component==0xff) return false;
    f->source_pair=static_cast<uint8_t>((word>>8)&0xffu);
    f->component=component;
    f->source_odd=source_odd;
    f->scalar=scalar;
    return true;
}

bool encode_vcomp_rcp_f32_semantic(const VcompRcpF32Semantic &i, uint64_t *word) {
    if (i.src.bank!=RegisterBank::PrimaryAttribute || i.component>=4 || ((i.src.num&1u) && i.component>=2))
        return false;
    return encode_vcomp_rcp_f32({static_cast<uint8_t>(i.src.num/2),i.component,(i.src.num&1u)!=0},word);
}

bool decode_vcomp_rcp_f32_semantic(uint64_t word, VcompRcpF32Semantic *i) {
    if (!i) return false;
    VcompRcpF32Fields f{};
    if (!decode_vcomp_rcp_f32(word,&f) || f.source_pair>127) return false;
    i->src={RegisterBank::PrimaryAttribute,static_cast<uint8_t>(f.source_pair*2+(f.source_odd?1:0))};
    i->component=f.component;
    return true;
}

bool encode_vcomp_rcp_scalar_f32_semantic(const VcompRcpScalarF32Semantic &i, uint64_t *word) {
    if (i.src.bank!=RegisterBank::PrimaryAttribute || i.src.num!=0 || i.component>=2) return false;
    return encode_vcomp_rcp_f32({0,i.component,false,true},word);
}

bool decode_vcomp_rcp_scalar_f32_semantic(uint64_t word, VcompRcpScalarF32Semantic *i) {
    if (!i) return false;
    VcompRcpF32Fields f{};
    if (!decode_vcomp_rcp_f32(word,&f) || !f.scalar || f.source_pair!=0 || f.component>=2) return false;
    i->src={RegisterBank::PrimaryAttribute,0};
    i->component=f.component;
    return true;
}

bool encode_vcomp_f32_semantic(const VcompF32Semantic &i, uint64_t *word) {
    if (!word || i.src_component >= 4 || i.dest_mask == 0 || i.dest_mask >= 16 ||
        i.dst.num >= 128 || i.src.num >= 128 ||
        (i.op != ComplexOp::Reciprocal && i.op != ComplexOp::Rsqrt &&
         i.op != ComplexOp::Log2 && i.op != ComplexOp::Exp2))
        return false;
    VcompFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_ext) ||
        !encode_src1_bank(i.src.bank,&f.src1_bank,&f.src1_ext)) return false;
    f.pred=static_cast<uint8_t>(Predicate::Always);
    f.skip_invalid=i.skip_invalid;
    // Exp2 uses the alternate destination-type selector in every independently
    // observed Sony profile; reciprocal/log2 use the base F32 selector.
    f.dest_type=i.op==ComplexOp::Exp2 && !i.exp2_base_dest_type ? 1 : 0;
    f.end=i.end;
    f.no_schedule=i.no_schedule;
    f.op2=static_cast<uint8_t>(i.op);
    f.src_type=0; // F32
    f.src1_mod=i.src_absolute ? 2 : 0;
    f.src_component=i.src_component;
    f.dest_num=i.dst.num;
    f.src1_num=i.src.num;
    f.write_mask=i.dest_mask;
    return encode_vcomp(f,word);
}

bool decode_vcomp_f32_semantic(uint64_t word, VcompF32Semantic *i) {
    if (!i) return false;
    VcompFields f{};
    if (!decode_vcomp(word,&f) || f.pred!=static_cast<uint8_t>(Predicate::Always) ||
        f.sync_start || f.repeat_count!=0 || f.src_type!=0 ||
        (f.src1_mod!=0 && f.src1_mod!=2) || f.src_component>=4 || f.write_mask==0 ||
        (f.op2!=static_cast<uint8_t>(ComplexOp::Reciprocal) &&
         f.op2!=static_cast<uint8_t>(ComplexOp::Rsqrt) &&
         f.op2!=static_cast<uint8_t>(ComplexOp::Log2) &&
         f.op2!=static_cast<uint8_t>(ComplexOp::Exp2))) return false;
    const auto op=static_cast<ComplexOp>(f.op2);
    if ((op==ComplexOp::Exp2 && f.dest_type>1) || (op!=ComplexOp::Exp2 && f.dest_type!=0))
        return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_ext,&i->dst.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_ext,&i->src.bank)) return false;
    i->op=op;
    i->dst.num=f.dest_num;
    i->src.num=f.src1_num;
    i->src_component=f.src_component;
    i->dest_mask=f.write_mask;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
    i->end=f.end;
    i->exp2_base_dest_type=op==ComplexOp::Exp2 && f.dest_type==0;
    i->src_absolute=f.src1_mod==2;
    return true;
}

bool encode_v16nmad_div_f32_semantic(const V16NmadDivF32Semantic &i, uint64_t *word) {
    if (!word) return false;
    if (i.components==1) *word=0x10a4008600040f7cULL;
    else if (i.components==2) *word=0x10a4418600040f7cULL;
    else if (i.components==3) *word=0x10a4438600040f7cULL;
    else if (i.components==4) *word=0x10a4478600040f7cULL;
    else return false;
    return true;
}

bool decode_v16nmad_div_f32_semantic(uint64_t word, V16NmadDivF32Semantic *i) {
    if (!i) return false;
    if (word==0x10a4008600040f7cULL) i->components=1;
    else if (word==0x10a4418600040f7cULL) i->components=2;
    else if (word==0x10a4438600040f7cULL) i->components=3;
    else if (word==0x10a4478600040f7cULL) i->components=4;
    else return false;
    return true;
}

bool encode_v16nmad_mul_pack_f32_semantic(const V16NmadMulPackF32Semantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x10a4478600040f7cULL;
    return true;
}

bool decode_v16nmad_mul_pack_f32_semantic(uint64_t word, V16NmadMulPackF32Semantic *i) {
    return i && word==0x10a4478600040f7cULL;
}

bool encode_v16nmad_phong_fragment_max_semantic(const V16NmadPhongFragmentMaxSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x10a4078600046f3dULL;
    return true;
}

bool decode_v16nmad_phong_fragment_max_semantic(uint64_t word, V16NmadPhongFragmentMaxSemantic *i) {
    return i && word==0x10a4078600046f3dULL;
}

bool encode_v16nmad_dot_splat_f32_semantic(const V16NmadDotSplatF32Semantic &i, uint64_t *word) {
    if (!word) return false;
    if (i.components==2) *word=0x10c0418a00047f7cULL;
    else if (i.components==3) *word=0x10c0f38600047f3dULL;
    else return false;
    return true;
}

bool decode_v16nmad_dot_splat_f32_semantic(uint64_t word, V16NmadDotSplatF32Semantic *i) {
    if (!i) return false;
    if (word==0x10c0418a00047f7cULL) i->components=2;
    else if (word==0x10c0f38600047f3dULL) i->components=3;
    else return false;
    return true;
}

bool encode_v16nmad_f32_to_s32_semantic(const V16NmadF32ToS32Semantic &i, uint64_t *word) {
    if (!word) return false;
    if (i.phase==0) *word=0x10a40084a0042000ULL;
    else if (i.phase==1) *word=0x10a400a620041000ULL;
    else return false;
    return true;
}

bool decode_v16nmad_f32_to_s32_semantic(uint64_t word, V16NmadF32ToS32Semantic *i) {
    if (!i) return false;
    if (word==0x10a40084a0042000ULL) i->phase=0;
    else if (word==0x10a400a620041000ULL) i->phase=1;
    else return false;
    return true;
}

bool encode_vpck_s32x2_color_semantic(const VpckS32x2ColorSemantic &i, uint64_t *word) {
    if (!word) return false;
    if (i.phase==0) *word=0x408106caa0000080ULL;
    else if (i.phase==1) *word=0x4085094ea0010000ULL;
    else return false;
    return true;
}

bool decode_vpck_s32x2_color_semantic(uint64_t word, VpckS32x2ColorSemantic *i) {
    if (!i) return false;
    if (word==0x408106caa0000080ULL) i->phase=0;
    else if (word==0x4085094ea0010000ULL) i->phase=1;
    else return false;
    return true;
}

bool encode_vpck_s32_to_f32_semantic(const VpckS32ToF32Semantic &i, uint64_t *word) {
    if (!word) return false;
    if (i.phase==0) *word=0x40810786a0c00081ULL;
    else if (i.phase==1) *word=0x40810786a0800080ULL;
    else return false;
    return true;
}

bool decode_vpck_s32_to_f32_semantic(uint64_t word, VpckS32ToF32Semantic *i) {
    if (!i) return false;
    if (word==0x40810786a0c00081ULL) i->phase=0;
    else if (word==0x40810786a0800080ULL) i->phase=1;
    else return false;
    return true;
}

bool encode_vmad2_s32_to_f32_semantic(const Vmad2S32ToF32Semantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x00800086a0403042ULL;
    return true;
}

bool decode_vmad2_s32_to_f32_semantic(uint64_t word, Vmad2S32ToF32Semantic *i) {
    return i && word==0x00800086a0403042ULL;
}

bool encode_vmad2_f32_scalar_mad_semantic(const Vmad2F32ScalarMadSemantic &i, uint64_t *word) {
    if (!word || i.predicate!=Predicate::Always || i.dest_mask==0 || i.dest_mask>=16 ||
        i.dst.bank!=RegisterBank::Temp || i.src0.bank!=RegisterBank::Temp ||
        i.src1.bank!=RegisterBank::SecondaryAttribute || i.src2.bank!=RegisterBank::SecondaryAttribute ||
        i.dst.num>=64 || i.src0.num>=64 || i.src1.num>=64 || i.src2.num>=64)
        return false;
    Vmad2Fields f{};
    f.data_f16=false;
    f.pred=0;
    f.skip_invalid=i.skip_invalid;
    f.no_schedule=i.no_schedule;
    f.dest_mask=i.dest_mask;
    f.src1_mod=i.src1_negative ? 1 : 0;
    f.dest_bank=0;
    f.src0_bank=false;
    f.src1_bank=3;
    f.src2_bank=3;
    f.dest_num=i.dst.num;
    f.src0_num=i.src0.num;
    f.src1_num=i.src1.num;
    f.src2_num=i.src2.num;
    f.src0_swizzle_01=0;       // xxxx
    f.src1_swizzle_01=1;       // yyyy
    f.src1_swizzle_bit2=false;
    f.src2_swizzle=0;          // xxxx
    return encode_vmad2(f,word);
}

bool decode_vmad2_f32_scalar_mad_semantic(uint64_t word, Vmad2F32ScalarMadSemantic *i) {
    if (!i) return false;
    Vmad2Fields f{};
    if (!decode_vmad2(word,&f) || f.data_f16 || f.pred!=0 || f.sync_start || f.src0_abs ||
        f.src1_bank_ext || f.src2_bank_ext || f.src2_swizzle!=0 || f.src1_swizzle_bit2 ||
        f.src2_mod!=0 || f.src0_bank || f.dest_bank!=0 || f.src1_bank!=3 || f.src2_bank!=3 ||
        f.src0_swizzle_bit2 || f.src0_swizzle_01!=0 || f.src1_swizzle_01!=1 ||
        (f.src1_mod!=0 && f.src1_mod!=1) || f.dest_mask==0)
        return false;
    i->dst={RegisterBank::Temp,f.dest_num};
    i->src0={RegisterBank::Temp,f.src0_num};
    i->src1={RegisterBank::SecondaryAttribute,f.src1_num};
    i->src2={RegisterBank::SecondaryAttribute,f.src2_num};
    i->predicate=Predicate::Always;
    i->dest_mask=f.dest_mask;
    i->src1_negative=f.src1_mod==1;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
    return true;
}

bool encode_vmad2_f32_semantic(const Vmad2F32Semantic &i, uint64_t *word) {
    if (!word || i.predicate!=Predicate::Always || i.dest_mask==0 || i.dest_mask>=16 ||
        i.dst.num>=64 || i.src0.num>=64 || i.src1.num>=64 || i.src2.num>=64)
        return false;
    Vmad2Fields f{};
    bool dest_ext=false,src0_ext=false;
    uint8_t src0_selector=0;
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&dest_ext) || dest_ext ||
        !encode_src0_bank(i.src0.bank,&src0_selector,&src0_ext) || src0_ext || src0_selector>1 ||
        !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_bank_ext) ||
        !encode_src1_bank(i.src2.bank,&f.src2_bank,&f.src2_bank_ext))
        return false;
    f.src0_bank=src0_selector!=0;
    uint8_t sw0=0,sw1=0,sw2=0;
    if (!encode_vmad2_swizzle(i.src0_swizzle,kVmad2Src0Swizzles,&sw0) ||
        !encode_vmad2_swizzle(i.src1_swizzle,kVmad2Src1Swizzles,&sw1) ||
        !encode_vmad2_swizzle(i.src2_swizzle,kVmad2Src2Swizzles,&sw2))
        return false;
    f.data_f16=false;
    f.pred=0;
    f.skip_invalid=i.skip_invalid;
    f.src0_abs=i.src0_absolute;
    f.no_schedule=i.no_schedule;
    f.dest_mask=i.dest_mask;
    f.src1_mod=(i.src1_negative?1u:0u)|(i.src1_absolute?2u:0u);
    f.src2_mod=(i.src2_negative?1u:0u)|(i.src2_absolute?2u:0u);
    f.dest_num=i.dst.num;
    f.src0_num=i.src0.num;
    f.src1_num=i.src1.num;
    f.src2_num=i.src2.num;
    f.src0_swizzle_01=sw0&3u;
    f.src0_swizzle_bit2=(sw0&4u)!=0;
    f.src1_swizzle_01=sw1&3u;
    f.src1_swizzle_bit2=(sw1&4u)!=0;
    f.src2_swizzle=sw2;
    return encode_vmad2(f,word);
}

bool decode_vmad2_f32_semantic(uint64_t word, Vmad2F32Semantic *i) {
    if (!i) return false;
    Vmad2Fields f{};
    if (!decode_vmad2(word,&f) || f.data_f16 || f.pred!=0 || f.sync_start ||
        f.dest_mask==0 || f.src1_mod>3 || f.src2_mod>3 || f.src2_swizzle>=8)
        return false;
    if (!decode_dest_bank(f.dest_bank,false,&i->dst.bank) ||
        !decode_src0_bank(f.src0_bank,false,&i->src0.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src1.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_bank_ext,&i->src2.bank))
        return false;
    const uint8_t sw0=static_cast<uint8_t>(f.src0_swizzle_01|(f.src0_swizzle_bit2?4u:0u));
    const uint8_t sw1=static_cast<uint8_t>(f.src1_swizzle_01|(f.src1_swizzle_bit2?4u:0u));
    if (sw0>=8 || sw1>=8) return false;
    i->dst.num=f.dest_num;
    i->src0.num=f.src0_num;
    i->src1.num=f.src1_num;
    i->src2.num=f.src2_num;
    i->predicate=Predicate::Always;
    i->dest_mask=f.dest_mask;
    i->src0_swizzle=kVmad2Src0Swizzles[sw0];
    i->src1_swizzle=kVmad2Src1Swizzles[sw1];
    i->src2_swizzle=kVmad2Src2Swizzles[f.src2_swizzle];
    i->src0_absolute=f.src0_abs;
    i->src1_negative=(f.src1_mod&1u)!=0;
    i->src1_absolute=(f.src1_mod&2u)!=0;
    i->src2_negative=(f.src2_mod&1u)!=0;
    i->src2_absolute=(f.src2_mod&2u)!=0;
    i->skip_invalid=f.skip_invalid;
    i->no_schedule=f.no_schedule;
    return true;
}

bool encode_vdual_f32_mul_move_semantic(const VdualF32MulMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x20c42000cfb61080ULL;
    return true;
}

bool decode_vdual_f32_mul_move_semantic(uint64_t word, VdualF32MulMoveSemantic *i) {
    return i && word==0x20c42000cfb61080ULL;
}

bool encode_vdual_f32_exp_move_semantic(const VdualF32ExpMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x20c54000a0150480ULL;
    return true;
}

bool decode_vdual_f32_exp_move_semantic(uint64_t word, VdualF32ExpMoveSemantic *i) {
    return i && word==0x20c54000a0150480ULL;
}

bool encode_vdual_fixed16_add_move_semantic(const VdualFixed16AddMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x28844000cfb61088ULL;
    return true;
}

bool decode_vdual_fixed16_add_move_semantic(uint64_t word, VdualFixed16AddMoveSemantic *i) {
    return i && word==0x28844000cfb61088ULL;
}

bool encode_vdual_f32_dot_move_semantic(const VdualF32DotMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x28c41511e0160b8cULL;
    return true;
}

bool decode_vdual_f32_dot_move_semantic(uint64_t word, VdualF32DotMoveSemantic *i) {
    return i && word==0x28c41511e0160b8cULL;
}

bool encode_vdual_f32_reciprocal_move_semantic(const VdualF32ReciprocalMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x28847000ef950096ULL;
    return true;
}

bool decode_vdual_f32_reciprocal_move_semantic(uint64_t word, VdualF32ReciprocalMoveSemantic *i) {
    return i && word==0x28847000ef950096ULL;
}

bool encode_vdual_smooth_f32_dot_move_semantic(const VdualSmoothF32DotMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x2084111210140088ULL;
    return true;
}

bool decode_vdual_smooth_f32_dot_move_semantic(uint64_t word, VdualSmoothF32DotMoveSemantic *i) {
    return i && word==0x2084111210140088ULL;
}

bool encode_vdual_smooth_f32_reciprocal_mul_semantic(const VdualSmoothF32ReciprocalMulSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x208071002f8c10fdULL;
    return true;
}

bool decode_vdual_smooth_f32_reciprocal_mul_semantic(uint64_t word, VdualSmoothF32ReciprocalMulSemantic *i) {
    return i && word==0x208071002f8c10fdULL;
}

bool encode_vdual_phong_fragment_f32_dot_move_semantic(const VdualPhongFragmentF32DotMoveSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x2004111290540080ULL;
    return true;
}

bool decode_vdual_phong_fragment_f32_dot_move_semantic(uint64_t word, VdualPhongFragmentF32DotMoveSemantic *i) {
    return i && word==0x2004111290540080ULL;
}

bool encode_vdual_phong_fragment_f32_reciprocal_mul_semantic(const VdualPhongFragmentF32ReciprocalMulSemantic &, uint64_t *word) {
    if (!word) return false;
    *word=0x200071002f8c10fdULL;
    return true;
}

bool decode_vdual_phong_fragment_f32_reciprocal_mul_semantic(uint64_t word, VdualPhongFragmentF32ReciprocalMulSemantic *i) {
    return i && word==0x200071002f8c10fdULL;
}

bool encode_smlsi_semantic(const SmlsiSemantic &i, uint64_t *word) {
    if (!word || i.temp_limit>=16 || i.primary_limit>=16 || i.secondary_limit>=16)
        return false;
    uint64_t v=0xfa10000000000000ULL;
    v|=static_cast<uint64_t>(i.no_schedule)<<50;
    v|=static_cast<uint64_t>(i.temp_limit)<<44;
    v|=static_cast<uint64_t>(i.primary_limit)<<40;
    v|=static_cast<uint64_t>(i.secondary_limit)<<36;
    v|=static_cast<uint64_t>(i.dest_inc_mode)<<35;
    v|=static_cast<uint64_t>(i.src0_inc_mode)<<34;
    v|=static_cast<uint64_t>(i.src1_inc_mode)<<33;
    v|=static_cast<uint64_t>(i.src2_inc_mode)<<32;
    v|=static_cast<uint64_t>(i.dest_inc)<<24;
    v|=static_cast<uint64_t>(i.src0_inc)<<16;
    v|=static_cast<uint64_t>(i.src1_inc)<<8;
    v|=static_cast<uint64_t>(i.src2_inc);
    *word=v;
    return true;
}

bool decode_smlsi_semantic(uint64_t word, SmlsiSemantic *i) {
    if (!i || (word&~0x0004ffffffffffffULL)!=0xfa10000000000000ULL)
        return false;
    i->no_schedule=((word>>50)&1u)!=0;
    i->temp_limit=static_cast<uint8_t>((word>>44)&0xfu);
    i->primary_limit=static_cast<uint8_t>((word>>40)&0xfu);
    i->secondary_limit=static_cast<uint8_t>((word>>36)&0xfu);
    i->dest_inc_mode=((word>>35)&1u)!=0;
    i->src0_inc_mode=((word>>34)&1u)!=0;
    i->src1_inc_mode=((word>>33)&1u)!=0;
    i->src2_inc_mode=((word>>32)&1u)!=0;
    i->dest_inc=static_cast<uint8_t>((word>>24)&0xffu);
    i->src0_inc=static_cast<uint8_t>((word>>16)&0xffu);
    i->src1_inc=static_cast<uint8_t>((word>>8)&0xffu);
    i->src2_inc=static_cast<uint8_t>(word&0xffu);
    return true;
}

bool encode_vmad_semantic(const VmadSemantic &i, uint64_t *word) {
    if (!word || i.dst.num>=64 || i.src1.num>=64 || i.gpi0>=4 || i.gpi1>=4 || i.write_mask>=16 || i.repeat_count>=4) return false;
    VmadFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_bank_ext) || !encode_src1_bank(i.src1.bank,&f.src1_bank,&f.src1_bank_ext)) return false;
    uint8_t g0=0,g1=0,s1=0;
    if (i.gpi0_one3_extended) {
        if (i.vec4) return false;
        g0=7;
        f.gpi0_swizzle_ext=true;
    } else if (!encode_std_swizzle(i.gpi0_swizzle,&g0)) return false;
    if (!encode_std_swizzle(i.src1_swizzle,&s1)) {
        // Smooth lighting uses VMAD3 source1 selector 4 from the extended
        // table: xy0, with the unused fourth lane canonicalized to X.
        if (i.vec4 || !same_swizzle(i.src1_swizzle,kVmad3Src1XY0)) return false;
        s1=4;
        f.src1_swizzle_ext=true;
    }
    if (i.gpi1_zero3_extended) {
        if (i.vec4) return false;
        g1=6;
        f.gpi1_swizzle_ext=true;
    } else if (!encode_std_swizzle(i.gpi1_swizzle,&g1)) return false;
    f.pred=static_cast<uint8_t>(i.predicate); f.skip_invalid=i.skip_invalid; f.control_bit_53=i.control_bit_53;
    f.opcode2=i.vec4; f.repeat_mode=static_cast<uint8_t>(i.repeat_mode); f.repeat_count=i.repeat_count;
    f.no_schedule=i.no_schedule; f.write_mask=i.write_mask; f.dest_num=i.dst.num; f.src1_num=i.src1.num; f.gpi0_num=i.gpi0; f.gpi1_num=i.gpi1;
    f.src1_neg=i.src1_negative;
    f.gpi0_swizzle=g0; f.gpi1_swizzle=g1; f.src1_swizzle=s1;
    return encode_vmad(f,word);
}

bool decode_vmad_semantic(uint64_t word, VmadSemantic *i) {
    if (!i) return false; VmadFields f{}; if (!decode_vmad(word,&f)) return false;
    // Keep extended swizzles limited to the oracle-observed VMAD3 forms.
    if (f.gpi0_abs || f.gpi0_neg ||
        f.gpi1_abs || f.gpi1_neg || f.src1_abs) return false;
    if (f.src1_swizzle_ext && (f.opcode2 || f.src1_swizzle!=4)) return false;
    if (f.gpi0_swizzle_ext && (f.opcode2 || f.gpi0_swizzle!=7)) return false;
    if (f.gpi1_swizzle_ext && (f.opcode2 || f.gpi1_swizzle!=6)) return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_bank_ext,&i->dst.bank) || !decode_src1_bank(f.src1_bank,f.src1_bank_ext,&i->src1.bank)) return false;
    i->dst.num=f.dest_num; i->src1.num=f.src1_num; i->predicate=static_cast<Predicate>(f.pred); i->gpi0=f.gpi0_num; i->gpi1=f.gpi1_num; i->write_mask=f.write_mask;
    i->gpi0_one3_extended=f.gpi0_swizzle_ext;
    i->gpi1_zero3_extended=f.gpi1_swizzle_ext;
    if (f.gpi0_swizzle_ext) {
        i->gpi0_swizzle={{SwizzleChannel::One,SwizzleChannel::One,SwizzleChannel::One,SwizzleChannel::X}};
    } else i->gpi0_swizzle=decode_std_swizzle(f.gpi0_swizzle);
    if (f.gpi1_swizzle_ext) {
        i->gpi1_swizzle={{SwizzleChannel::Zero,SwizzleChannel::Zero,SwizzleChannel::Zero,SwizzleChannel::X}};
    } else i->gpi1_swizzle=decode_std_swizzle(f.gpi1_swizzle);
    i->src1_swizzle=f.src1_swizzle_ext ? kVmad3Src1XY0 : decode_std_swizzle(f.src1_swizzle);
    i->vec4=f.opcode2; i->control_bit_53=f.control_bit_53; i->repeat_mode=static_cast<RepeatMode>(f.repeat_mode);
    i->src1_negative=f.src1_neg;
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

bool encode_vtst_f32_lane_less_scalar_semantic(const VtstF32LaneLessScalarSemantic &i,
                                                uint64_t *word) {
    if (!word || i.vector_lane.num>=128 || i.scalar.num>=128 ||
        i.predicate_destination>=4 || i.lane>=2 ||
        (i.op!=CompareOp::Less && i.op!=CompareOp::NotEqual)) return false;
    VtstFields f{};
    if (!encode_src1_bank(i.vector_lane.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.scalar.bank,&f.src2_bank,&f.src2_ext)) return false;
    f.pred=static_cast<uint8_t>(i.predicate);
    f.skip_invalid=i.skip_invalid;
    f.control_bit_54=i.control_bit_54;
    f.dest_ext=true;
    f.precision=true;
    f.src2_vector_scalar_component=true;
    if (i.op==CompareOp::Less) {
        // The vector/scalar less-than profile uses the observed reversed
        // subtract test: zero=1/sign=2/crcomb=OR.
        f.zero_test=1;
        f.sign_test=2;
        f.test_crcomb_and=false;
    } else {
        // Smooth-lighting SDK 3.0 uses vector/scalar VSUB != 0.
        f.zero_test=2;
        f.sign_test=0;
        f.test_crcomb_and=true;
    }
    f.channel=i.lane;
    f.predicate_destination=i.predicate_destination;
    f.dest_bank=1;
    f.dest_num=0;
    f.test_write_enable=false;
    f.alu_select=0;
    f.alu_op=14;
    f.src1_num=i.vector_lane.num;
    f.src2_num=i.scalar.num;
    return encode_vtst(f,word);
}

bool decode_vtst_f32_lane_less_scalar_semantic(uint64_t word,
                                                VtstF32LaneLessScalarSemantic *i) {
    if (!i) return false;
    VtstFields f{};
    if (!decode_vtst(word,&f) || !f.dest_ext || f.dest_bank!=1 || f.dest_num!=0 ||
        f.test_write_enable || f.alu_select!=0 || f.alu_op!=14 || !f.precision ||
        f.src1_negative || !f.src2_vector_scalar_component || f.repeat_count!=0 ||
        f.once_only || f.sync_start || f.channel>=2) return false;
    if (f.zero_test==1 && f.sign_test==2 && !f.test_crcomb_and)
        i->op=CompareOp::Less;
    else if (f.zero_test==2 && f.sign_test==0 && f.test_crcomb_and)
        i->op=CompareOp::NotEqual;
    else return false;
    if (!decode_src1_bank(f.src1_bank,f.src1_ext,&i->vector_lane.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->scalar.bank)) return false;
    i->vector_lane.num=f.src1_num;
    i->scalar.num=f.src2_num;
    i->predicate=static_cast<Predicate>(f.pred);
    i->predicate_destination=f.predicate_destination;
    i->lane=f.channel;
    i->skip_invalid=f.skip_invalid;
    i->control_bit_54=f.control_bit_54;
    return true;
}

bool encode_vtst_f32_max_nonzero_value_semantic(const VtstF32MaxNonzeroValueSemantic &i,
                                                 uint64_t *word) {
    if (!word || i.dst.num>=128 || i.src.num>=128 || i.rhs.num>=128) return false;
    VtstFields f{};
    if (!encode_dest_bank(i.dst.bank,&f.dest_bank,&f.dest_ext) ||
        !encode_src1_bank(i.src.bank,&f.src1_bank,&f.src1_ext) ||
        !encode_src1_bank(i.rhs.bank,&f.src2_bank,&f.src2_ext)) return false;
    f.pred=static_cast<uint8_t>(Predicate::Always);
    f.skip_invalid=i.skip_invalid;
    f.control_bit_54=true;
    f.precision=true;
    f.zero_test=2;
    f.sign_test=0;
    f.test_crcomb_and=true;
    f.channel=0;
    f.predicate_destination=0;
    f.dest_num=i.dst.num;
    f.test_write_enable=true;
    f.alu_select=0;
    f.alu_op=10; // VMAX F32 in the public Vita3K test-ALU table.
    f.src1_num=i.src.num;
    f.src2_num=i.rhs.num;
    return encode_vtst(f,word);
}

bool decode_vtst_f32_max_nonzero_value_semantic(uint64_t word,
                                                 VtstF32MaxNonzeroValueSemantic *i) {
    if (!i) return false;
    VtstFields f{};
    if (!decode_vtst(word,&f) || f.pred!=static_cast<uint8_t>(Predicate::Always) ||
        !f.control_bit_54 || f.once_only || f.sync_start ||
        !f.precision || f.src1_negative || f.src2_vector_scalar_component || f.repeat_count!=0 ||
        f.sign_test!=0 || f.zero_test!=2 || !f.test_crcomb_and || f.channel!=0 ||
        f.predicate_destination!=0 || !f.test_write_enable || f.alu_select!=0 || f.alu_op!=10)
        return false;
    if (!decode_dest_bank(f.dest_bank,f.dest_ext,&i->dst.bank) ||
        !decode_src1_bank(f.src1_bank,f.src1_ext,&i->src.bank) ||
        !decode_src1_bank(f.src2_bank,f.src2_ext,&i->rhs.bank)) return false;
    i->dst.num=f.dest_num;
    i->src.num=f.src1_num;
    i->rhs.num=f.src2_num;
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
    f.end=i.end;
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
    if (!decode_vbw(word,&f) || f.partial || f.repeat_select || f.sync_start)
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
    i->end=f.end;
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
    if (!word || i.control_payload > 0x0fffffffu) return false;
    KillFields f{};
    if (!encode_short_predicate(i.predicate,&f.short_predicate)) return false;
    f.dontcare_payload=i.control_payload;
    return encode_kill(f,word);
}

bool decode_kill_semantic(uint64_t word, KillSemantic *i) {
    if (!i) return false;
    KillFields f{};
    if (!decode_kill(word,&f)) return false;
    if (!decode_short_predicate(f.short_predicate,&i->predicate)) return false;
    i->control_payload=f.dontcare_payload;
    return true;
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
