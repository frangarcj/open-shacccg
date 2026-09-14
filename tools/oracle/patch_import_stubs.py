#!/usr/bin/env python3
"""Patch Vita import stubs into ARM SVC traps for a local emulator harness.

Input is a *private* user-supplied oracle ELF plus the JSON produced by
vita_imports.py. The patched binary is a local research artifact and must not be
redistributed with open-shacccg.
"""
import argparse
import json
import pathlib
import struct

ARM_SVC_BASE = 0xEF000000
ARM_BX_LR = 0xE12FFF1E


def load_segments(raw):
    eh = struct.unpack_from('<16sHHIIIIIHHHHHH', raw, 0)
    _, _, _, _, _, phoff, _, _, _, phentsz, phnum, _, _, _ = eh
    out = []
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from(
            '<IIIIIIII', raw, phoff + i * phentsz)
        if ptype == 1:
            out.append((vaddr, vaddr + filesz, off))
    return out


def va_to_file(segments, va, size=1):
    for start, end, file_off in segments:
        if start <= va and va + size <= end:
            return file_off + (va - start)
    raise ValueError(f'VA 0x{va:08X} is not backed by a PT_LOAD file range')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('imports_json')
    ap.add_argument('output_elf')
    ap.add_argument('trap_map')
    args = ap.parse_args()

    raw = bytearray(pathlib.Path(args.elf).read_bytes())
    report = json.loads(pathlib.Path(args.imports_json).read_text())
    segments = load_segments(raw)
    trap = 1
    trap_rows = []

    for lib in report['imports']:
        names = lib.get('function_names') or [None] * len(lib.get('function_nids', []))
        for nid, name, va in zip(lib.get('function_nids', []), names, lib.get('function_entries', [])):
            if trap > 0x00FFFFFF:
                raise SystemExit('too many trap IDs for ARM SVC immediate')
            off = va_to_file(segments, va, 8)
            before = bytes(raw[off:off + 16])
            struct.pack_into('<II', raw, off, ARM_SVC_BASE | trap, ARM_BX_LR)
            trap_rows.append({
                'trap': trap,
                'library': lib.get('library'),
                'library_nid': lib.get('library_nid'),
                'function_nid': nid,
                'function_name': name,
                'stub_va': va,
                'original_stub_hex': before.hex(),
            })
            trap += 1

    pathlib.Path(args.output_elf).write_bytes(raw)
    pathlib.Path(args.trap_map).write_text(json.dumps({'traps': trap_rows}, indent=2))
    print(f'patched {len(trap_rows)} import stubs -> {args.output_elf}')


if __name__ == '__main__':
    main()
