#!/usr/bin/env python3
"""Declarative raw USSE encodings used by gen_usse_raw.py.

Patterns run from bit 63 to bit 0. Binary tokens are fixed bits; NAME:WIDTH
entries are fields copied to/from the corresponding *Fields struct.

This file is project-owned clean-room data. External decoders and public shader
traces are used only to cross-check observable bit layouts.
"""

ENCODINGS = [
    {
        "name": "vmov",
        "struct": "VmovFields",
        "pattern": """
            00111 pred:3 skip_invalid:1 test_bit_2:1 src0_component_select:1
            sync_start:1 dest_bank_ext:1 end_or_src0_bank_ext:1 src1_bank_ext:1
            src2_bank_ext:1 move_type:2 repeat_count:2 no_schedule:1 data_type:3
            test_bit_1:1 src0_swizzle:4 src0_bank:1 dest_bank:2 src1_bank:2
            src2_bank:2 dest_mask:4 dest_num:6 src0_num:6 src1_num:6 src2_num:6
        """,
    },
    {
        "name": "vpck",
        "struct": "VpckFields",
        "pattern": """
            01000 pred:3 skip_invalid:1 no_schedule:1 unknown:1 sync_start:1
            dest_bank_ext:1 end:1 src1_bank_ext:1 src2_bank_ext:1 repeat_count:4
            src_format:3 dest_format:3 dest_mask:4 dest_bank:2 src1_bank:2
            src2_bank:2 dest_num:7 component3:2 scale:1 component1:2 component2:2
            src1_num:6 component0_bit1:1 src2_num:6 component0_bit0:1
        """,
    },
    {
        "name": "v32nmad",
        "struct": "V32NmadFields",
        "pattern": """
            00001 pred:3 skip_invalid:1 src1_swizzle_10_11:2 sync_start:1
            dest_bank_ext:1 src1_swizzle_9:1 src1_bank_ext:1 src2_bank_ext:1
            src2_swizzle:4 no_schedule:1 dest_mask:4 src1_mod:2 src2_mod:1
            src1_swizzle_7_8:2 dest_bank:2 src1_bank:2 src2_bank:2 dest_num:6
            src1_swizzle_0_6:7 op2:3 src1_num:6 src2_num:6
        """,
    },
    {
        "name": "vmad",
        "struct": "VmadFields",
        "pattern": """
            00011 pred:3 skip_invalid:1 gpi1_swizzle_ext:1 1 opcode2:1
            dest_bank_ext:1 end:1 src1_bank_ext:1 repeat_mode:2 gpi0_abs:1
            repeat_count:2 no_schedule:1 write_mask:4 src1_neg:1 src1_abs:1
            gpi1_neg:1 gpi1_abs:1 gpi0_swizzle_ext:1 dest_bank:2 src1_bank:2
            gpi0_num:2 dest_num:6 gpi0_swizzle:4 gpi1_swizzle:4 gpi1_num:2
            gpi0_neg:1 src1_swizzle_ext:1 src1_swizzle:4 src1_num:6
        """,
    },
    {
        "name": "vtst",
        "struct": "VtstFields",
        "pattern": """
            01001 pred:3 skip_invalid:1 x once_only:1 sync_start:1 dest_ext:1
            src1_negative:1 src1_ext:1 src2_ext:1 precision:1
            src2_vector_scalar_component:1 repeat_count:2 sign_test:2 zero_test:2
            test_crcomb_and:1 channel:3 predicate_destination:2 dest_bank:2
            src1_bank:2 src2_bank:2 dest_num:7 test_write_enable:1 alu_select:2
            alu_op:4 src1_num:7 src2_num:7
        """,
    },
    {
        "name": "vbw",
        "struct": "VbwFields",
        "pattern": """
            01 op1:3 pred:3 skip_invalid:1 no_schedule:1 repeat_select:1
            sync_start:1 dest_ext:1 end:1 src1_ext:1 src2_ext:1 repeat_count:4
            src2_invert:1 src2_rotate:5 src2_extra_high:2 op2:1 partial:1
            dest_bank:2 src1_bank:2 src2_bank:2 dest_num:7 src2_select:7
            src1_num:7 src2_num:7
        """,
        "constraints": [
            {"field": "op1", "min": 2, "max": 6},
        ],
    },
    {
        "name": "kill",
        "struct": "KillFields",
        "pattern": """
            11111 001 dontcare_top:2 11 000000000 short_predicate:2
            0000001101111 dontcare_payload:28
        """,
    },
]
