#pragma once

#include "usse/compact_encoding.hpp"
#include "usse/usse.hpp"

namespace vsc::usse::detail {

#define VSC_FIELD(type, member, offset, width) BitField<&type::member, offset, width>

using VmovEncoding = Encoding<0xf800000000000000ULL, 0x3800000000000000ULL,
    VSC_FIELD(VmovFields, pred, 56, 3), VSC_FIELD(VmovFields, skip_invalid, 55, 1),
    VSC_FIELD(VmovFields, test_bit_2, 54, 1), VSC_FIELD(VmovFields, src0_component_select, 53, 1),
    VSC_FIELD(VmovFields, sync_start, 52, 1), VSC_FIELD(VmovFields, dest_bank_ext, 51, 1),
    VSC_FIELD(VmovFields, end_or_src0_bank_ext, 50, 1), VSC_FIELD(VmovFields, src1_bank_ext, 49, 1),
    VSC_FIELD(VmovFields, src2_bank_ext, 48, 1), VSC_FIELD(VmovFields, move_type, 46, 2),
    VSC_FIELD(VmovFields, repeat_count, 44, 2), VSC_FIELD(VmovFields, no_schedule, 43, 1),
    VSC_FIELD(VmovFields, data_type, 40, 3), VSC_FIELD(VmovFields, test_bit_1, 39, 1),
    VSC_FIELD(VmovFields, src0_swizzle, 35, 4), VSC_FIELD(VmovFields, src0_bank, 34, 1),
    VSC_FIELD(VmovFields, dest_bank, 32, 2), VSC_FIELD(VmovFields, src1_bank, 30, 2),
    VSC_FIELD(VmovFields, src2_bank, 28, 2), VSC_FIELD(VmovFields, dest_mask, 24, 4),
    VSC_FIELD(VmovFields, dest_num, 18, 6), VSC_FIELD(VmovFields, src0_num, 12, 6),
    VSC_FIELD(VmovFields, src1_num, 6, 6), VSC_FIELD(VmovFields, src2_num, 0, 6)>;

using VpckEncoding = Encoding<0xf800000000000000ULL, 0x4000000000000000ULL,
    VSC_FIELD(VpckFields, pred, 56, 3), VSC_FIELD(VpckFields, skip_invalid, 55, 1),
    VSC_FIELD(VpckFields, no_schedule, 54, 1), VSC_FIELD(VpckFields, unknown, 53, 1),
    VSC_FIELD(VpckFields, sync_start, 52, 1), VSC_FIELD(VpckFields, dest_bank_ext, 51, 1),
    VSC_FIELD(VpckFields, end, 50, 1), VSC_FIELD(VpckFields, src1_bank_ext, 49, 1),
    VSC_FIELD(VpckFields, src2_bank_ext, 48, 1), VSC_FIELD(VpckFields, repeat_count, 44, 4),
    VSC_FIELD(VpckFields, src_format, 41, 3), VSC_FIELD(VpckFields, dest_format, 38, 3),
    VSC_FIELD(VpckFields, dest_mask, 34, 4), VSC_FIELD(VpckFields, dest_bank, 32, 2),
    VSC_FIELD(VpckFields, src1_bank, 30, 2), VSC_FIELD(VpckFields, src2_bank, 28, 2),
    VSC_FIELD(VpckFields, dest_num, 21, 7), VSC_FIELD(VpckFields, component3, 19, 2),
    VSC_FIELD(VpckFields, scale, 18, 1), VSC_FIELD(VpckFields, component1, 16, 2),
    VSC_FIELD(VpckFields, component2, 14, 2), VSC_FIELD(VpckFields, src1_num, 8, 6),
    VSC_FIELD(VpckFields, component0_bit1, 7, 1), VSC_FIELD(VpckFields, src2_num, 1, 6),
    VSC_FIELD(VpckFields, component0_bit0, 0, 1)>;

using V32NmadEncoding = Encoding<0xf800000000000000ULL, 0x0800000000000000ULL,
    VSC_FIELD(V32NmadFields, pred, 56, 3), VSC_FIELD(V32NmadFields, skip_invalid, 55, 1),
    VSC_FIELD(V32NmadFields, src1_swizzle_10_11, 53, 2), VSC_FIELD(V32NmadFields, sync_start, 52, 1),
    VSC_FIELD(V32NmadFields, dest_bank_ext, 51, 1), VSC_FIELD(V32NmadFields, src1_swizzle_9, 50, 1),
    VSC_FIELD(V32NmadFields, src1_bank_ext, 49, 1), VSC_FIELD(V32NmadFields, src2_bank_ext, 48, 1),
    VSC_FIELD(V32NmadFields, src2_swizzle, 44, 4), VSC_FIELD(V32NmadFields, no_schedule, 43, 1),
    VSC_FIELD(V32NmadFields, dest_mask, 39, 4), VSC_FIELD(V32NmadFields, src1_mod, 37, 2),
    VSC_FIELD(V32NmadFields, src2_mod, 36, 1), VSC_FIELD(V32NmadFields, src1_swizzle_7_8, 34, 2),
    VSC_FIELD(V32NmadFields, dest_bank, 32, 2), VSC_FIELD(V32NmadFields, src1_bank, 30, 2),
    VSC_FIELD(V32NmadFields, src2_bank, 28, 2), VSC_FIELD(V32NmadFields, dest_num, 22, 6),
    VSC_FIELD(V32NmadFields, src1_swizzle_0_6, 15, 7), VSC_FIELD(V32NmadFields, op2, 12, 3),
    VSC_FIELD(V32NmadFields, src1_num, 6, 6), VSC_FIELD(V32NmadFields, src2_num, 0, 6)>;

using VcompEncoding = Encoding<0xf800000000000000ULL, 0x3000000000000000ULL,
    VSC_FIELD(VcompFields, pred, 56, 3), VSC_FIELD(VcompFields, skip_invalid, 55, 1),
    VSC_FIELD(VcompFields, dest_type, 53, 2), VSC_FIELD(VcompFields, sync_start, 52, 1),
    VSC_FIELD(VcompFields, dest_ext, 51, 1), VSC_FIELD(VcompFields, end, 50, 1),
    VSC_FIELD(VcompFields, src1_ext, 49, 1), VSC_FIELD(VcompFields, repeat_count, 44, 4),
    VSC_FIELD(VcompFields, no_schedule, 43, 1), VSC_FIELD(VcompFields, op2, 41, 2),
    VSC_FIELD(VcompFields, src_type, 39, 2), VSC_FIELD(VcompFields, src1_mod, 37, 2),
    VSC_FIELD(VcompFields, src_component, 35, 2), VSC_FIELD(VcompFields, dest_bank, 32, 2),
    VSC_FIELD(VcompFields, src1_bank, 30, 2), VSC_FIELD(VcompFields, dest_num, 21, 7),
    VSC_FIELD(VcompFields, src1_num, 7, 7), VSC_FIELD(VcompFields, write_mask, 0, 4)>;

using VmadEncoding = Encoding<0xf800000000000000ULL, 0x1800000000000000ULL,
    VSC_FIELD(VmadFields, pred, 56, 3), VSC_FIELD(VmadFields, skip_invalid, 55, 1),
    VSC_FIELD(VmadFields, gpi1_swizzle_ext, 54, 1), VSC_FIELD(VmadFields, control_bit_53, 53, 1),
    VSC_FIELD(VmadFields, opcode2, 52, 1),
    VSC_FIELD(VmadFields, dest_bank_ext, 51, 1), VSC_FIELD(VmadFields, end, 50, 1),
    VSC_FIELD(VmadFields, src1_bank_ext, 49, 1), VSC_FIELD(VmadFields, repeat_mode, 47, 2),
    VSC_FIELD(VmadFields, gpi0_abs, 46, 1), VSC_FIELD(VmadFields, repeat_count, 44, 2),
    VSC_FIELD(VmadFields, no_schedule, 43, 1), VSC_FIELD(VmadFields, write_mask, 39, 4),
    VSC_FIELD(VmadFields, src1_neg, 38, 1), VSC_FIELD(VmadFields, src1_abs, 37, 1),
    VSC_FIELD(VmadFields, gpi1_neg, 36, 1), VSC_FIELD(VmadFields, gpi1_abs, 35, 1),
    VSC_FIELD(VmadFields, gpi0_swizzle_ext, 34, 1), VSC_FIELD(VmadFields, dest_bank, 32, 2),
    VSC_FIELD(VmadFields, src1_bank, 30, 2), VSC_FIELD(VmadFields, gpi0_num, 28, 2),
    VSC_FIELD(VmadFields, dest_num, 22, 6), VSC_FIELD(VmadFields, gpi0_swizzle, 18, 4),
    VSC_FIELD(VmadFields, gpi1_swizzle, 14, 4), VSC_FIELD(VmadFields, gpi1_num, 12, 2),
    VSC_FIELD(VmadFields, gpi0_neg, 11, 1), VSC_FIELD(VmadFields, src1_swizzle_ext, 10, 1),
    VSC_FIELD(VmadFields, src1_swizzle, 6, 4), VSC_FIELD(VmadFields, src1_num, 0, 6)>;

using VtstEncoding = Encoding<0xf800000000000000ULL, 0x4800000000000000ULL,
    VSC_FIELD(VtstFields, pred, 56, 3), VSC_FIELD(VtstFields, skip_invalid, 55, 1),
    VSC_FIELD(VtstFields, once_only, 53, 1), VSC_FIELD(VtstFields, sync_start, 52, 1),
    VSC_FIELD(VtstFields, dest_ext, 51, 1), VSC_FIELD(VtstFields, src1_negative, 50, 1),
    VSC_FIELD(VtstFields, src1_ext, 49, 1), VSC_FIELD(VtstFields, src2_ext, 48, 1),
    VSC_FIELD(VtstFields, precision, 47, 1), VSC_FIELD(VtstFields, src2_vector_scalar_component, 46, 1),
    VSC_FIELD(VtstFields, repeat_count, 44, 2), VSC_FIELD(VtstFields, sign_test, 42, 2),
    VSC_FIELD(VtstFields, zero_test, 40, 2), VSC_FIELD(VtstFields, test_crcomb_and, 39, 1),
    VSC_FIELD(VtstFields, channel, 36, 3), VSC_FIELD(VtstFields, predicate_destination, 34, 2),
    VSC_FIELD(VtstFields, dest_bank, 32, 2), VSC_FIELD(VtstFields, src1_bank, 30, 2),
    VSC_FIELD(VtstFields, src2_bank, 28, 2), VSC_FIELD(VtstFields, dest_num, 21, 7),
    VSC_FIELD(VtstFields, test_write_enable, 20, 1), VSC_FIELD(VtstFields, alu_select, 18, 2),
    VSC_FIELD(VtstFields, alu_op, 14, 4), VSC_FIELD(VtstFields, src1_num, 7, 7),
    VSC_FIELD(VtstFields, src2_num, 0, 7)>;

using VbwEncoding = Encoding<0xc000000000000000ULL, 0x4000000000000000ULL,
    VSC_FIELD(VbwFields, op1, 59, 3), VSC_FIELD(VbwFields, pred, 56, 3),
    VSC_FIELD(VbwFields, skip_invalid, 55, 1), VSC_FIELD(VbwFields, no_schedule, 54, 1),
    VSC_FIELD(VbwFields, repeat_select, 53, 1), VSC_FIELD(VbwFields, sync_start, 52, 1),
    VSC_FIELD(VbwFields, dest_ext, 51, 1), VSC_FIELD(VbwFields, end, 50, 1),
    VSC_FIELD(VbwFields, src1_ext, 49, 1), VSC_FIELD(VbwFields, src2_ext, 48, 1),
    VSC_FIELD(VbwFields, repeat_count, 44, 4), VSC_FIELD(VbwFields, src2_invert, 43, 1),
    VSC_FIELD(VbwFields, src2_rotate, 38, 5), VSC_FIELD(VbwFields, src2_extra_high, 36, 2),
    VSC_FIELD(VbwFields, op2, 35, 1), VSC_FIELD(VbwFields, partial, 34, 1),
    VSC_FIELD(VbwFields, dest_bank, 32, 2), VSC_FIELD(VbwFields, src1_bank, 30, 2),
    VSC_FIELD(VbwFields, src2_bank, 28, 2), VSC_FIELD(VbwFields, dest_num, 21, 7),
    VSC_FIELD(VbwFields, src2_select, 14, 7), VSC_FIELD(VbwFields, src1_num, 7, 7),
    VSC_FIELD(VbwFields, src2_num, 0, 7)>;

using KillEncoding = Encoding<0xff3ff9fff0000000ULL, 0xf9300006f0000000ULL,
    VSC_FIELD(KillFields, dontcare_top, 54, 2), VSC_FIELD(KillFields, short_predicate, 41, 2),
    VSC_FIELD(KillFields, dontcare_payload, 0, 28)>;

using BranchEncoding = Encoding<0xf8fffffffff00000ULL, 0xf800004000000000ULL,
    VSC_FIELD(BranchFields, pred, 56, 3), VSC_FIELD(BranchFields, offset, 0, 20)>;

using I32Mad2Encoding = Encoding<0xf8000c7800000000ULL, 0xd000000000000000ULL,
    VSC_FIELD(I32Mad2Fields, pred, 56, 3), VSC_FIELD(I32Mad2Fields, dontcare, 55, 1),
    VSC_FIELD(I32Mad2Fields, no_schedule, 54, 1), VSC_FIELD(I32Mad2Fields, sn, 52, 2),
    VSC_FIELD(I32Mad2Fields, dest_ext, 51, 1), VSC_FIELD(I32Mad2Fields, end, 50, 1),
    VSC_FIELD(I32Mad2Fields, src1_ext, 49, 1), VSC_FIELD(I32Mad2Fields, src2_ext, 48, 1),
    VSC_FIELD(I32Mad2Fields, src0_ext, 47, 1), VSC_FIELD(I32Mad2Fields, count, 44, 3),
    VSC_FIELD(I32Mad2Fields, is_signed, 41, 1), VSC_FIELD(I32Mad2Fields, negative_src1, 40, 1),
    VSC_FIELD(I32Mad2Fields, negative_src2, 39, 1), VSC_FIELD(I32Mad2Fields, src0_bank, 34, 1),
    VSC_FIELD(I32Mad2Fields, dest_bank, 32, 2), VSC_FIELD(I32Mad2Fields, src1_bank, 30, 2),
    VSC_FIELD(I32Mad2Fields, src2_bank, 28, 2), VSC_FIELD(I32Mad2Fields, dest_num, 21, 7),
    VSC_FIELD(I32Mad2Fields, src0_num, 14, 7), VSC_FIELD(I32Mad2Fields, src1_num, 7, 7),
    VSC_FIELD(I32Mad2Fields, src2_num, 0, 7)>;

#undef VSC_FIELD

} // namespace vsc::usse::detail
