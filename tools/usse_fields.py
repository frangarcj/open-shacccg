#!/usr/bin/env python3
"""Decode validated raw USSE2 field layouts from a Vita GXP image.

This is intentionally a factual/raw decoder: bank, format and swizzle numbers
are printed numerically until their semantic mappings are independently proven.
"""
import argparse
import struct
from pathlib import Path

from gxp_info import parse


def get(w, o, n):
    return (w >> o) & ((1 << n) - 1)


def decode(word):
    major = get(word, 59, 5)
    if major == 0x07:
        return "VMOV", {
            "pred":get(word,56,3), "skip":get(word,55,1), "move_type":get(word,46,2),
            "repeat":get(word,44,2), "dtype":get(word,40,3), "swiz0":get(word,35,4),
            "dst_bank":get(word,32,2), "src1_bank":get(word,30,2), "src2_bank":get(word,28,2),
            "mask":get(word,24,4), "dst":get(word,18,6), "src0":get(word,12,6),
            "src1":get(word,6,6), "src2":get(word,0,6),
        }
    if major == 0x08:
        return "VPCK", {
            "pred":get(word,56,3), "skip":get(word,55,1), "nosched":get(word,54,1),
            "end":get(word,50,1), "repeat":get(word,44,4), "src_fmt":get(word,41,3),
            "dst_fmt":get(word,38,3), "mask":get(word,34,4), "dst_bank":get(word,32,2),
            "src1_bank":get(word,30,2), "src2_bank":get(word,28,2), "dst":get(word,21,7),
            "c3":get(word,19,2), "scale":get(word,18,1), "c1":get(word,16,2),
            "c2":get(word,14,2), "src1":get(word,8,6), "c0b1":get(word,7,1),
            "src2":get(word,1,6), "c0b0":get(word,0,1),
        }
    if major == 0x01:
        return "V32NMAD", {
            "pred":get(word,56,3), "skip":get(word,55,1), "src2_swiz":get(word,44,4),
            "nosched":get(word,43,1), "mask":get(word,39,4), "src1_mod":get(word,37,2),
            "src2_mod":get(word,36,1), "dst_bank":get(word,32,2), "src1_bank":get(word,30,2),
            "src2_bank":get(word,28,2), "dst":get(word,22,6), "op2":get(word,12,3),
            "src1":get(word,6,6), "src2":get(word,0,6),
        }
    if major == 0x03 and get(word,53,1) == 1:
        return "VMAD", {
            "pred":get(word,56,3), "skip":get(word,55,1), "op2":get(word,52,1),
            "end":get(word,50,1), "repeat_mode":get(word,47,2), "repeat":get(word,44,2),
            "nosched":get(word,43,1), "mask":get(word,39,4), "dst_bank":get(word,32,2),
            "src1_bank":get(word,30,2), "gpi0":get(word,28,2), "dst":get(word,22,6),
            "gpi0_swiz":get(word,18,4), "gpi1_swiz":get(word,14,4), "gpi1":get(word,12,2),
            "src1_swiz":get(word,6,4), "src1":get(word,0,6),
        }
    return None, {}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('gxp')
    args = ap.parse_args()
    report = parse(Path(args.gxp).read_bytes())
    for phase in ('primary_program','secondary_program'):
        block = report[phase]
        print(f"{phase}: {block['instruction_count']}")
        for row in block['instructions']:
            word = int(row['qword'], 16) if isinstance(row['qword'], str) else row['qword']
            name, fields = decode(word)
            label = name or 'unvalidated'
            tail = ' '.join(f'{k}={v}' for k,v in fields.items())
            print(f"  {row['index']:02d}: {word:016x} {label} {tail}")

if __name__ == '__main__':
    main()
