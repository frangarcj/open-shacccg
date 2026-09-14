#!/usr/bin/env python3
"""Print coarse USSE2 instruction families from a Vita GXP image.

This is deliberately a major-opcode workbench, not a full decoder. It only
labels independently tracked top-level instruction categories and leaves all
unvalidated field decoding untouched.
"""
import argparse
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import gxp_info

NAMES = {
    0x00: 'VMAD2',
    0x01: 'V32NMAD',
    0x02: 'V16NMAD',
    0x03: 'VMAD/VDP',
    0x04: 'VDUAL',
    0x05: 'VDUAL',
    0x06: 'VCOMP',
    0x07: 'VMOV',
    0x08: 'VPCK',
    0x09: 'VTST',
    0x0a: 'VBW',
    0x0b: 'VBW',
    0x0c: 'VBW',
    0x0d: 'VBW',
    0x0e: 'VBW',
    0x0f: 'VTSTMSK',
    0x1c: 'SMP',
    0x1f: 'CONTROL',
}


def classify(row):
    word = int(row['qword'], 16)
    major = (word >> 59) & 0x1f
    name = NAMES.get(major, 'UNKNOWN')
    if major == 0x1f:
        op2 = (word >> 56) & 0x7
        opcat = (word >> 52) & 0x3
        if op2 == 0x2 and ((word >> 52) & 0x7) == 0x4:
            name = 'PHAS'
        elif ((word >> 54) & 1) == 0 and opcat == 0 and ((word >> 38) & 0x7) == 0x5:
            name = 'NOP'
        elif op2 == 0x3 and opcat == 0x2:
            name = 'EMIT/SPEC'
        elif op2 == 0x1 and opcat == 0x3 and ((word >> 43) & 0x1ff) == 0 and ((word >> 28) & 0x1fff) == 0x06f:
            name = 'KILL'
        elif ((word >> 54) & 1) == 0 and opcat == 0:
            name = 'BR'
    return major, name


def main():
    ap=argparse.ArgumentParser(description='Classify USSE2 instruction families in a GXP')
    ap.add_argument('gxp', type=pathlib.Path)
    ns=ap.parse_args()
    r=gxp_info.parse(ns.gxp.read_bytes())
    print(f"{ns.gxp}: {r['stage']}")
    for phase in ('secondary_program','primary_program'):
        block=r[phase]
        print(f"{phase.replace('_program','')}: {block['instruction_count']} instructions")
        for row in block['instructions']:
            major,name=classify(row)
            print(f"  {row['index']:02d} +0x{row['offset']:04x} {row['qword']} major=0x{major:02x} {name}")

if __name__ == '__main__':
    main()
