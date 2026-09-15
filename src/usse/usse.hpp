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

// Two PHAS control words are independently observed in Sony 1.6.5 output:
// mode 1 starts control/discard work and mode 7 resumes the normal main phase.
// Keep the field semantic-but-opaque until the hardware meaning is known.
enum class PhaseMode : uint8_t { Control = 1, Main = 7 };

struct PhaseSemantic {
    PhaseMode mode = PhaseMode::Main;
};

struct NopSemantic {
    bool no_schedule = true;
    bool end = false;
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
    bool end = false;
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

// Oracle-anchored VCOMP reciprocal profile used by F32 vector division. This
// intentionally exposes only the source PA pair and scalar component proven by
// a/b vs b/a differential probes; other VCOMP operations remain fail-closed.
struct VcompRcpF32Fields {
    uint8_t source_pair = 0;
    uint8_t component = 0;
    bool source_odd = false;
    bool scalar = false;
};

struct VcompRcpF32Semantic {
    RegisterRef src{}; // PrimaryAttribute vector base; odd PA is validated for float2 xy
    uint8_t component = 0;
};

struct VcompRcpScalarF32Semantic {
    RegisterRef src{}; // currently oracle-validated for PA0 only
    uint8_t component = 0; // X/Y packed scalar input
};

enum class ComplexOp : uint8_t {
    Reciprocal = 0,
    Rsqrt = 1,
    Log2 = 2,
    Exp2 = 3,
};

// Field-level VCOMP shape independently anchored by reciprocal and Log2
// differential probes. The current semantic surface deliberately stays on
// scalar F32, but unlike the older division-only helpers it permits the
// observed TEMP/PA/SA bank combinations needed by real vertex expressions.
struct VcompF32Semantic {
    ComplexOp op = ComplexOp::Reciprocal;
    RegisterRef dst{};
    RegisterRef src{};
    uint8_t src_component = 0;
    uint8_t dest_mask = 1;
    bool skip_invalid = true;
    bool no_schedule = false;
    bool end = false;
};

// Fixed F32 division combine forms observed after reciprocal VCOMPs and
// numerator staging. Kept narrow until the general V16NMAD layout is anchored.
struct V16NmadDivF32Semantic { uint8_t components = 4; };

// Fixed four-lane multiply/pack form observed in SDK 3.0.0 texture-tint
// fragments after staging the uniform in TEMP124 and the sampled color in
// TEMP125. Keep it distinct from division even though the final raw word is
// shared by the currently validated four-lane forms.
struct V16NmadMulPackF32Semantic {};

// Fixed final reductions for the oracle dot-splat float2/float3 profiles.
struct V16NmadDotSplatF32Semantic { uint8_t components = 2; };

// Two fixed V16NMAD phases used by Sony after scalar F32 staging when the
// fragment result is converted to signed integer COLOR. Kept opaque until a
// broader V16NMAD field layout is independently anchored.
struct V16NmadF32ToS32Semantic { uint8_t phase = 0; };

// Fixed two-word VPCK sequence used to expose one packed int2 COLOR result.
// Kept separate from generic VPCK because integer component numbering aliases
// selector fields that are not fully characterized yet.
struct VpckS32x2ColorSemantic { uint8_t phase = 0; };

// Two fixed U16->F32 unpack steps used by Sony's scalar signed-int-to-float
// conversion profile. The integer VPCK component selectors are intentionally
// kept opaque until broader integer pack coverage is independently anchored.
struct VpckS32ToF32Semantic { uint8_t phase = 0; };

// Fixed VMAD2 core step in the same scalar S32->F32 conversion profile. The
// general VMAD2 field layout remains outside the validated semantic surface.
struct Vmad2S32ToF32Semantic {};

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
    // Oracle evidence shows both values are legal. Public libvita2d VMADs use
    // 1; the compact repeated mat4 profile emitted by Sony uses 0. The exact
    // hardware meaning of this control bit is intentionally left unnamed.
    bool control_bit_53 = true;
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

// Oracle-validated F32 compare profile. This uses the VTST floating subtract
// ALU rather than the scalar-U32 test profile above.
struct VtstF32Semantic {
    RegisterRef lhs{};
    RegisterRef rhs{};
    Predicate predicate = Predicate::Always;
    CompareOp op = CompareOp::Equal;
    uint8_t predicate_destination = 0;
    uint8_t component = 0;
    bool skip_invalid = false;
};

// Oracle-validated packed F32 lane < scalar comparison. Sony uses the VTST
// vector/scalar source2 form for e.g. `color.a < uniform_cut`, with the vector
// lane supplied by one PA register and the scalar by an SA register.
struct VtstF32LaneLessScalarSemantic {
    RegisterRef vector_lane{};
    RegisterRef scalar{};
    Predicate predicate = Predicate::Always;
    uint8_t predicate_destination = 0;
    uint8_t lane = 0; // x/y lane within `vector_lane`
    bool skip_invalid = true;
};

// Oracle-validated signed-32 loop compare. The first supported form is the
// glslang/SceShaccCg `i < n` profile used by a dynamic for-loop.
struct VtstS32Semantic {
    RegisterRef lhs{};
    RegisterRef rhs{};
    Predicate predicate = Predicate::Always;
    CompareOp op = CompareOp::Less;
    uint8_t predicate_destination = 0;
    bool skip_invalid = true;
};

// Oracle-validated I32MAD2 subset used for loop-counter update/feed-through.
// The semantic layer deliberately exposes only sn=0/1, unsigned operands and
// unmodified scalar registers until more oracle cases establish other forms.
struct I32Mad2Semantic {
    RegisterRef dst{};
    RegisterRef src0{};
    RegisterRef src1{};
    RegisterRef src2{};
    uint8_t sn = 0;
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
    bool end = false;
};

struct KillSemantic {
    Predicate predicate = Predicate::Always;
};

// Evidence-backed control BR form captured from the original SceShaccCg 1.6.5
// compiler. Offsets are signed instruction deltas relative to the BR itself.
struct BranchSemantic {
    Predicate predicate = Predicate::Always;
    int32_t offset = 0;
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
    bool control_bit_53 = true;
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

struct VcompFields {
    uint8_t pred = 0;
    bool skip_invalid = false;
    uint8_t dest_type = 0;
    bool sync_start = false;
    bool dest_ext = false;
    bool end = false;
    bool src1_ext = false;
    uint8_t repeat_count = 0;
    bool no_schedule = false;
    uint8_t op2 = 0;
    uint8_t src_type = 0;
    uint8_t src1_mod = 0;
    uint8_t src_component = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t dest_num = 0;
    uint8_t src1_num = 0;
    uint8_t write_mask = 0;
};

struct KillFields {
    uint8_t dontcare_top = 0;
    uint8_t short_predicate = 0;
    uint32_t dontcare_payload = 0;
};

struct BranchFields {
    uint8_t pred = 0;
    uint32_t offset = 0; // raw signed-20-bit two's-complement payload
};

struct I32Mad2Fields {
    uint8_t pred = 0;
    bool dontcare = false;
    bool no_schedule = false;
    uint8_t sn = 0;
    bool dest_ext = false;
    bool end = false;
    bool src1_ext = false;
    bool src2_ext = false;
    bool src0_ext = false;
    uint8_t count = 0;
    bool is_signed = false;
    bool negative_src1 = false;
    bool negative_src2 = false;
    uint8_t src0_bank = 0;
    uint8_t dest_bank = 0;
    uint8_t src1_bank = 0;
    uint8_t src2_bank = 0;
    uint8_t dest_num = 0;
    uint8_t src0_num = 0;
    uint8_t src1_num = 0;
    uint8_t src2_num = 0;
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
    Vtst, Vbw, VtstMask, I32Mad2, Sample, Control, Unknown,
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
bool decode_vcomp(uint64_t word, VcompFields *fields);
bool encode_vcomp(const VcompFields &fields, uint64_t *word);
bool decode_vcomp_rcp_f32(uint64_t word, VcompRcpF32Fields *fields);
bool encode_vcomp_rcp_f32(const VcompRcpF32Fields &fields, uint64_t *word);
bool decode_vmad(uint64_t word, VmadFields *fields);
bool encode_vmad(const VmadFields &fields, uint64_t *word);
bool decode_vtst(uint64_t word, VtstFields *fields);
bool encode_vtst(const VtstFields &fields, uint64_t *word);
bool decode_vbw(uint64_t word, VbwFields *fields);
bool encode_vbw(const VbwFields &fields, uint64_t *word);
bool decode_kill(uint64_t word, KillFields *fields);
bool encode_kill(const KillFields &fields, uint64_t *word);
bool decode_branch(uint64_t word, BranchFields *fields);
bool encode_branch(const BranchFields &fields, uint64_t *word);
bool decode_i32mad2(uint64_t word, I32Mad2Fields *fields);
bool encode_i32mad2(const I32Mad2Fields &fields, uint64_t *word);

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
bool encode_phase_semantic(const PhaseSemantic &instruction, uint64_t *word);
bool decode_phase_semantic(uint64_t word, PhaseSemantic *instruction);
bool encode_nop_semantic(const NopSemantic &instruction, uint64_t *word);
bool decode_nop_semantic(uint64_t word, NopSemantic *instruction);
bool encode_vpck_semantic(const VpckSemantic &instruction, uint64_t *word);
bool decode_vpck_semantic(uint64_t word, VpckSemantic *instruction);
bool encode_v32nmad_semantic(const V32NmadSemantic &instruction, uint64_t *word);
bool decode_v32nmad_semantic(uint64_t word, V32NmadSemantic *instruction);
bool encode_vcomp_rcp_f32_semantic(const VcompRcpF32Semantic &instruction, uint64_t *word);
bool decode_vcomp_rcp_f32_semantic(uint64_t word, VcompRcpF32Semantic *instruction);
bool encode_vcomp_rcp_scalar_f32_semantic(const VcompRcpScalarF32Semantic &instruction, uint64_t *word);
bool decode_vcomp_rcp_scalar_f32_semantic(uint64_t word, VcompRcpScalarF32Semantic *instruction);
bool encode_vcomp_f32_semantic(const VcompF32Semantic &instruction, uint64_t *word);
bool decode_vcomp_f32_semantic(uint64_t word, VcompF32Semantic *instruction);
bool encode_v16nmad_div_f32_semantic(const V16NmadDivF32Semantic &, uint64_t *word);
bool decode_v16nmad_div_f32_semantic(uint64_t word, V16NmadDivF32Semantic *instruction);
bool encode_v16nmad_mul_pack_f32_semantic(const V16NmadMulPackF32Semantic &, uint64_t *word);
bool decode_v16nmad_mul_pack_f32_semantic(uint64_t word, V16NmadMulPackF32Semantic *instruction);
bool encode_v16nmad_dot_splat_f32_semantic(const V16NmadDotSplatF32Semantic &, uint64_t *word);
bool decode_v16nmad_dot_splat_f32_semantic(uint64_t word, V16NmadDotSplatF32Semantic *instruction);
bool encode_v16nmad_f32_to_s32_semantic(const V16NmadF32ToS32Semantic &, uint64_t *word);
bool decode_v16nmad_f32_to_s32_semantic(uint64_t word, V16NmadF32ToS32Semantic *instruction);
bool encode_vpck_s32x2_color_semantic(const VpckS32x2ColorSemantic &, uint64_t *word);
bool decode_vpck_s32x2_color_semantic(uint64_t word, VpckS32x2ColorSemantic *instruction);
bool encode_vpck_s32_to_f32_semantic(const VpckS32ToF32Semantic &, uint64_t *word);
bool decode_vpck_s32_to_f32_semantic(uint64_t word, VpckS32ToF32Semantic *instruction);
bool encode_vmad2_s32_to_f32_semantic(const Vmad2S32ToF32Semantic &, uint64_t *word);
bool decode_vmad2_s32_to_f32_semantic(uint64_t word, Vmad2S32ToF32Semantic *instruction);
bool encode_vmad_semantic(const VmadSemantic &instruction, uint64_t *word);
bool decode_vmad_semantic(uint64_t word, VmadSemantic *instruction);
bool encode_vtst_semantic(const VtstSemantic &instruction, uint64_t *word);
bool decode_vtst_semantic(uint64_t word, VtstSemantic *instruction);
bool encode_vtst_f32_semantic(const VtstF32Semantic &instruction, uint64_t *word);
bool decode_vtst_f32_semantic(uint64_t word, VtstF32Semantic *instruction);
bool encode_vtst_f32_lane_less_scalar_semantic(const VtstF32LaneLessScalarSemantic &instruction, uint64_t *word);
bool decode_vtst_f32_lane_less_scalar_semantic(uint64_t word, VtstF32LaneLessScalarSemantic *instruction);
bool encode_vtst_s32_semantic(const VtstS32Semantic &instruction, uint64_t *word);
bool decode_vtst_s32_semantic(uint64_t word, VtstS32Semantic *instruction);
bool encode_i32mad2_semantic(const I32Mad2Semantic &instruction, uint64_t *word);
bool decode_i32mad2_semantic(uint64_t word, I32Mad2Semantic *instruction);
bool encode_vbw_semantic(const VbwSemantic &instruction, uint64_t *word);
bool decode_vbw_semantic(uint64_t word, VbwSemantic *instruction);
bool encode_kill_semantic(const KillSemantic &instruction, uint64_t *word);
bool decode_kill_semantic(uint64_t word, KillSemantic *instruction);
bool encode_branch_semantic(const BranchSemantic &instruction, uint64_t *word);
bool decode_branch_semantic(uint64_t word, BranchSemantic *instruction);

inline bool encode_semantic(const VmovSemantic &i, uint64_t *word) { return encode_vmov_semantic(i, word); }
inline bool encode_semantic(const PhaseSemantic &i, uint64_t *word) { return encode_phase_semantic(i, word); }
inline bool encode_semantic(const NopSemantic &i, uint64_t *word) { return encode_nop_semantic(i, word); }
inline bool encode_semantic(const VpckSemantic &i, uint64_t *word) { return encode_vpck_semantic(i, word); }
inline bool encode_semantic(const V32NmadSemantic &i, uint64_t *word) { return encode_v32nmad_semantic(i, word); }
inline bool encode_semantic(const VcompRcpF32Semantic &i, uint64_t *word) { return encode_vcomp_rcp_f32_semantic(i, word); }
inline bool encode_semantic(const VcompRcpScalarF32Semantic &i, uint64_t *word) { return encode_vcomp_rcp_scalar_f32_semantic(i, word); }
inline bool encode_semantic(const VcompF32Semantic &i, uint64_t *word) { return encode_vcomp_f32_semantic(i, word); }
inline bool encode_semantic(const V16NmadDivF32Semantic &i, uint64_t *word) { return encode_v16nmad_div_f32_semantic(i, word); }
inline bool encode_semantic(const V16NmadMulPackF32Semantic &i, uint64_t *word) { return encode_v16nmad_mul_pack_f32_semantic(i, word); }
inline bool encode_semantic(const V16NmadDotSplatF32Semantic &i, uint64_t *word) { return encode_v16nmad_dot_splat_f32_semantic(i, word); }
inline bool encode_semantic(const V16NmadF32ToS32Semantic &i, uint64_t *word) { return encode_v16nmad_f32_to_s32_semantic(i, word); }
inline bool encode_semantic(const VpckS32x2ColorSemantic &i, uint64_t *word) { return encode_vpck_s32x2_color_semantic(i, word); }
inline bool encode_semantic(const VpckS32ToF32Semantic &i, uint64_t *word) { return encode_vpck_s32_to_f32_semantic(i, word); }
inline bool encode_semantic(const Vmad2S32ToF32Semantic &i, uint64_t *word) { return encode_vmad2_s32_to_f32_semantic(i, word); }
inline bool encode_semantic(const VmadSemantic &i, uint64_t *word) { return encode_vmad_semantic(i, word); }
inline bool encode_semantic(const VtstSemantic &i, uint64_t *word) { return encode_vtst_semantic(i, word); }
inline bool encode_semantic(const VtstF32Semantic &i, uint64_t *word) { return encode_vtst_f32_semantic(i, word); }
inline bool encode_semantic(const VtstF32LaneLessScalarSemantic &i, uint64_t *word) { return encode_vtst_f32_lane_less_scalar_semantic(i, word); }
inline bool encode_semantic(const VtstS32Semantic &i, uint64_t *word) { return encode_vtst_s32_semantic(i, word); }
inline bool encode_semantic(const I32Mad2Semantic &i, uint64_t *word) { return encode_i32mad2_semantic(i, word); }
inline bool encode_semantic(const VbwSemantic &i, uint64_t *word) { return encode_vbw_semantic(i, word); }
inline bool encode_semantic(const KillSemantic &i, uint64_t *word) { return encode_kill_semantic(i, word); }
inline bool encode_semantic(const BranchSemantic &i, uint64_t *word) { return encode_branch_semantic(i, word); }

// Strict convenience encoder: unsupported semantic instructions fail rather
// than emitting guessed code.
bool encode(const Instruction &instruction, uint64_t *word);

} // namespace vsc::usse
