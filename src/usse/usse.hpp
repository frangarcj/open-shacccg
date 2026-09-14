#pragma once

#include <cstdint>

namespace vsc::usse {

enum class Stage : uint8_t { Vertex, Fragment };

enum class Opcode : uint16_t {
    Phase, Nop, Emit, Mov, Add, Mul, Mad, Dot, Sample, Branch, BranchCond, End
};

// Semantic register classes. USSE2 uses context-sensitive compact bank
// selectors, so these are deliberately separate from the raw 1/2-bit fields.
enum class RegisterBank : uint8_t {
    Temp, PrimaryAttribute, Output, SecondaryAttribute, Special, Immediate,
    Index, Indexed1, Indexed2, Invalid
};

enum class Predicate : uint8_t {
    Always = 0,
    P0 = 1,
    P1 = 2,
    P2 = 3,
    P3 = 4,
    NotP0 = 5,
    NotP1 = 6,
    PN = 7,
};

enum class DataType : uint8_t {
    S8 = 0, S16 = 1, S32 = 2, C10 = 3, F16 = 4, F32 = 5, U8 = 6, U16 = 7
};

struct RegisterRef {
    RegisterBank bank = RegisterBank::Invalid;
    uint8_t num = 0;
};

struct VmovSemantic {
    RegisterRef dst{};
    RegisterRef src{};
    Predicate predicate = Predicate::Always;
    DataType data_type = DataType::F32;
    uint8_t dest_mask = 0xF;
    uint8_t swizzle = 4; // validated standard identity encoding for vec4
    uint8_t repeat_count = 0;
    bool skip_invalid = true;
    bool no_schedule = false;
};

// VPCK has its own 3-bit format namespace.
enum class PackFormat : uint8_t {
    U8 = 0, S8 = 1, O8 = 2, U16 = 3, S16 = 4, F16 = 5, F32 = 6, C10 = 7
};

struct VpckSemantic {
    RegisterRef dst{};
    RegisterRef src1{};
    RegisterRef src2{};
    Predicate predicate = Predicate::Always;
    PackFormat src_format = PackFormat::F32;
    PackFormat dst_format = PackFormat::F32;
    uint8_t dest_mask = 0xF;
    uint8_t components[4] = {0, 1, 2, 3};
    uint8_t repeat_count = 0;
    bool scale = false;
    bool skip_invalid = true;
    bool no_schedule = true;
    bool end = false;
};


// F32 vector arithmetic operation encoded by V32NMAD.
enum class VectorOp : uint8_t { Mul = 0, Add = 1, Frac = 2, Ddx = 3, Ddy = 4, Min = 5, Max = 6, Dot = 7 };

// Hardware swizzles can also select constants. 0..3 are xyzw, 4=0, 5=1,
// 6=2, 7=0.5. Keeping this semantic channel enum lets IR-facing code avoid
// manipulating the packed 12-bit encoding directly.
enum class SwizzleChannel : uint8_t { X=0, Y=1, Z=2, W=3, Zero=4, One=5, Two=6, Half=7 };

struct Swizzle4 {
    SwizzleChannel c[4] = {SwizzleChannel::X, SwizzleChannel::Y, SwizzleChannel::Z, SwizzleChannel::W};
};

struct V32NmadSemantic {
    VectorOp op = VectorOp::Mul;
    RegisterRef dst{};
    RegisterRef src1{};
    RegisterRef src2{};
    Predicate predicate = Predicate::Always;
    uint8_t dest_mask = 0xF;
    Swizzle4 src1_swizzle{};
    Swizzle4 src2_swizzle{};
    bool src1_negative = false;
    bool src1_absolute = false;
    bool src2_absolute = false;
    bool skip_invalid = true;
    bool no_schedule = false;
};

enum class RepeatMode : uint8_t { External=0, Internal=1, Both=2, Slmsi=3 };

// VMAD is a three-input FMA where two inputs are GPI/FP-internal registers.
// This builder covers the standard (non-extended-swizzle) F32 VMAD3/4 forms
// used by the public vita2d matrix shaders.
struct VmadSemantic {
    RegisterRef dst{};
    RegisterRef src1{};
    Predicate predicate = Predicate::Always;
    uint8_t gpi0 = 0;
    uint8_t gpi1 = 0;
    uint8_t write_mask = 0xF;
    Swizzle4 gpi0_swizzle{};
    Swizzle4 src1_swizzle{};
    Swizzle4 gpi1_swizzle{};
    bool vec4 = true;
    RepeatMode repeat_mode = RepeatMode::Slmsi;
    uint8_t repeat_count = 0;
    bool skip_invalid = true;
    bool no_schedule = false;
};

enum class CompareOp : uint8_t {
    Equal,
    NotEqual,
    Less,
    LessEqual,
    Greater,
    GreaterEqual,
};

// Evidence-backed VTST form used for scalar unsigned-32 comparisons. The
// result is written to one of the four predicate registers and can predicate
// following USSE instructions.
struct VtstSemantic {
    RegisterRef lhs{};
    RegisterRef rhs{};
    Predicate predicate = Predicate::Always;
    CompareOp op = CompareOp::Equal;
    uint8_t predicate_destination = 0;
    uint8_t component = 0;
    bool skip_invalid = true;
};

enum class BitwiseOp : uint8_t {
    And,
    Or,
    Xor,
    ShiftLeft,
    RotateLeft,
    ShiftRight,
    ArithmeticShiftRight,
};

struct VbwSemantic {
    BitwiseOp op = BitwiseOp::Or;
    RegisterRef dst{};
    RegisterRef src1{};
    RegisterRef src2{};
    Predicate predicate = Predicate::Always;
    bool src2_is_immediate = false;
    uint32_t immediate = 0;
    uint8_t repeat_count = 0;
    bool skip_invalid = true;
    bool no_schedule = false;
};

struct KillSemantic {
    Predicate predicate = Predicate::Always;
};

struct VmovFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    bool test_bit_2 = false;
    bool src0_component_select = false;
    bool sync_start = false;
    bool dest_bank_ext = false;
    bool end_or_src0_bank_ext = false;
    bool src1_bank_ext = false;
    bool src2_bank_ext = false;
    uint8_t move_type = 0;
    uint8_t repeat_count = 0;
    bool no_schedule = false;
    uint8_t data_type = 0;
    bool test_bit_1 = false;
    uint8_t src0_swizzle = 0;
    uint8_t src0_bank = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_mask = 0;
    uint8_t dest_num = 0;
    uint8_t src0_num = 0;
    uint8_t src1_num = 0;
    uint8_t src2_num = 0;
};

struct VpckFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    bool no_schedule = false;
    bool unknown = false;
    bool sync_start = false;
    bool dest_bank_ext = false;
    bool end = false;
    bool src1_bank_ext = false;
    bool src2_bank_ext = false;
    uint8_t repeat_count = 0;
    uint8_t src_format = 0;
    uint8_t dest_format = 0;
    uint8_t dest_mask = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_num = 0;
    uint8_t component3 = 0;
    bool scale = false;
    uint8_t component1 = 0;
    uint8_t component2 = 0;
    uint8_t src1_num = 0;
    bool component0_bit1 = false;
    uint8_t src2_num = 0;
    bool component0_bit0 = false;
};

struct V32NmadFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    uint8_t src1_swizzle_10_11 = 0;
    bool sync_start = false;
    bool dest_bank_ext = false;
    bool src1_swizzle_9 = false;
    bool src1_bank_ext = false;
    bool src2_bank_ext = false;
    uint8_t src2_swizzle = 0;
    bool no_schedule = false;
    uint8_t dest_mask = 0;
    uint8_t src1_mod = 0;
    bool src2_mod = false;
    uint8_t src1_swizzle_7_8 = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_num = 0;
    uint8_t src1_swizzle_0_6 = 0;
    uint8_t op2 = 0;
    uint8_t src1_num = 0;
    uint8_t src2_num = 0;
};

struct VmadFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    bool gpi1_swizzle_ext = false;
    bool opcode2 = false;
    bool dest_bank_ext = false;
    bool end = false;
    bool src1_bank_ext = false;
    uint8_t repeat_mode = 0;
    bool gpi0_abs = false;
    uint8_t repeat_count = 0;
    bool no_schedule = false;
    uint8_t write_mask = 0;
    bool src1_neg = false;
    bool src1_abs = false;
    bool gpi1_neg = false;
    bool gpi1_abs = false;
    bool gpi0_swizzle_ext = false;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t gpi0_num = 0;
    uint8_t dest_num = 0;
    uint8_t gpi0_swizzle = 0;
    uint8_t gpi1_swizzle = 0;
    uint8_t gpi1_num = 0;
    bool gpi0_neg = false;
    bool src1_swizzle_ext = false;
    uint8_t src1_swizzle = 0;
    uint8_t src1_num = 0;
};

struct VtstFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    bool once_only = false;
    bool sync_start = false;
    bool dest_ext = false;
    bool src1_negative = false;
    bool src1_ext = false;
    bool src2_ext = false;
    bool precision = false;
    bool src2_vector_scalar_component = false;
    uint8_t repeat_count = 0;
    uint8_t sign_test = 0;
    uint8_t zero_test = 0;
    bool test_crcomb_and = false;
    uint8_t channel = 0;
    uint8_t predicate_destination = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_num = 0;
    bool test_write_enable = false;
    uint8_t alu_select = 0;
    uint8_t alu_op = 0;
    uint8_t src1_num = 0;
    uint8_t src2_num = 0;
};

struct VbwFields {
    uint8_t op1 = 0;
    uint8_t pred = 0;
    bool skip_invalid = false;
    bool no_schedule = false;
    bool repeat_select = false;
    bool sync_start = false;
    bool dest_ext = false;
    bool end = false;
    bool src1_ext = false;
    bool src2_ext = false;
    uint8_t repeat_count = 0;
    bool src2_invert = false;
    uint8_t src2_rotate = 0;
    uint8_t src2_extra_high = 0;
    bool op2 = false;
    bool partial = false;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_num = 0;
    uint8_t src2_select = 0;
    uint8_t src1_num = 0;
    uint8_t src2_num = 0;
};

struct KillFields {
    uint8_t dontcare_top = 0;
    uint8_t short_predicate = 0;
    uint32_t dontcare_payload = 0;
};

struct Instruction {
    Opcode opcode = Opcode::End;
    uint16_t dst = 0;
    uint16_t src0 = 0;
    uint16_t src1 = 0;
    uint16_t src2 = 0;
};

enum class ControlClass : uint8_t {
    Phase, Nop, Emit, LoadImmediate, Branch, Kill, Other, NotControl,
};

enum class MajorClass : uint8_t {
    Vmad2, V32Nmad, V16Nmad, VectorMadDot, Vdual, Vcomp, Vmov, Vpck,
    Vtst, Vbw, VtstMask, Sample, Control, Unknown,
};

uint8_t major_opcode(uint64_t word);
MajorClass classify_major(uint64_t word);
const char *major_class_name(MajorClass cls);
ControlClass classify_control(uint64_t word);
const char *control_class_name(ControlClass cls);

// Raw field codecs. Bank, type, format and swizzle values intentionally remain
// numeric until their semantic mappings have independent validation.
bool decode_vmov(uint64_t word, VmovFields *fields);
bool encode_vmov(const VmovFields &fields, uint64_t *word);
bool decode_vpck(uint64_t word, VpckFields *fields);
bool encode_vpck(const VpckFields &fields, uint64_t *word);
bool decode_v32nmad(uint64_t word, V32NmadFields *fields);
bool encode_v32nmad(const V32NmadFields &fields, uint64_t *word);
bool decode_vmad(uint64_t word, VmadFields *fields);
bool encode_vmad(const VmadFields &fields, uint64_t *word);
bool decode_vtst(uint64_t word, VtstFields *fields);
bool encode_vtst(const VtstFields &fields, uint64_t *word);
bool decode_vbw(uint64_t word, VbwFields *fields);
bool encode_vbw(const VbwFields &fields, uint64_t *word);
bool decode_kill(uint64_t word, KillFields *fields);
bool encode_kill(const KillFields &fields, uint64_t *word);

// Context-sensitive bank conversion helpers. These expose semantics without
// changing the raw codecs above.
bool encode_dest_bank(RegisterBank bank, uint8_t *selector, bool *extended);
bool decode_dest_bank(uint8_t selector, bool extended, RegisterBank *bank);
bool encode_src1_bank(RegisterBank bank, uint8_t *selector, bool *extended);
bool decode_src1_bank(uint8_t selector, bool extended, RegisterBank *bank);

// First semantic instruction builder. It emits the unconditional VMOV form
// used by the public Vita corpus while retaining explicit scheduling/repeat
// controls for differential testing.
bool encode_vmov_semantic(const VmovSemantic &instruction, uint64_t *word);
bool decode_vmov_semantic(uint64_t word, VmovSemantic *instruction);
bool encode_vpck_semantic(const VpckSemantic &instruction, uint64_t *word);
bool decode_vpck_semantic(uint64_t word, VpckSemantic *instruction);
bool encode_v32nmad_semantic(const V32NmadSemantic &instruction, uint64_t *word);
bool decode_v32nmad_semantic(uint64_t word, V32NmadSemantic *instruction);
bool encode_vmad_semantic(const VmadSemantic &instruction, uint64_t *word);
bool decode_vmad_semantic(uint64_t word, VmadSemantic *instruction);
bool encode_vtst_semantic(const VtstSemantic &instruction, uint64_t *word);
bool decode_vtst_semantic(uint64_t word, VtstSemantic *instruction);
bool encode_vbw_semantic(const VbwSemantic &instruction, uint64_t *word);
bool decode_vbw_semantic(uint64_t word, VbwSemantic *instruction);
bool encode_kill_semantic(const KillSemantic &instruction, uint64_t *word);
bool decode_kill_semantic(uint64_t word, KillSemantic *instruction);

// Strict convenience encoder: unsupported semantic instructions fail rather
// than emitting guessed code.
bool encode(const Instruction &instruction, uint64_t *word);

} // namespace vsc::usse
