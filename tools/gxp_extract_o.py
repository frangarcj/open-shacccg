#!/usr/bin/env python3
"""Extract a raw GXP payload from an ELF relocatable object produced by objcopy.

The common Vita shader build flow wraps a .gxp blob in an ARM ELF .o whose
`.data` section is the exact binary SceGxmProgram image. This extractor has no
VitaSDK dependency and is useful for public reference shader objects.
"""
from __future__ import annotations
import argparse, struct
from pathlib import Path


def extract_data(blob: bytes) -> bytes:
    if blob[:4] != b'\x7fELF' or blob[4] != 1 or blob[5] != 1:
        raise ValueError('expected little-endian ELF32 object')
    e_shoff = struct.unpack_from('<I', blob, 32)[0]
    e_shentsize = struct.unpack_from('<H', blob, 46)[0]
    e_shnum = struct.unpack_from('<H', blob, 48)[0]
    e_shstrndx = struct.unpack_from('<H', blob, 50)[0]
    if not e_shoff or not e_shentsize or e_shstrndx >= e_shnum:
        raise ValueError('ELF has no usable section table')

    def sh(i: int):
        off = e_shoff + i * e_shentsize
        if off + 40 > len(blob):
            raise ValueError('truncated section table')
        return struct.unpack_from('<IIIIIIIIII', blob, off)

    shstr = sh(e_shstrndx)
    names_off, names_size = shstr[4], shstr[5]
    names = blob[names_off:names_off + names_size]

    def name_at(n: int) -> str:
        end = names.find(b'\0', n)
        if end < 0: end = len(names)
        return names[n:end].decode('ascii', 'replace')

    for i in range(e_shnum):
        sec = sh(i)
        if name_at(sec[0]) == '.data':
            off, size = sec[4], sec[5]
            if off + size > len(blob):
                raise ValueError('truncated .data section')
            return blob[off:off + size]
    raise ValueError('ELF object has no .data section')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('object', type=Path)
    ap.add_argument('output', type=Path)
    ns = ap.parse_args()
    payload = extract_data(ns.object.read_bytes())
    ns.output.write_bytes(payload)
    print(f'extracted {len(payload)} bytes -> {ns.output}')

if __name__ == '__main__':
    main()
