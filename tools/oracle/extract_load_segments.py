#!/usr/bin/env python3
"""Extract PT_LOAD segments from the Vita ARM oracle ELF for emulator harnesses."""
import argparse
import json
import pathlib
import struct


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("out")
    args = ap.parse_args()
    data = pathlib.Path(args.elf).read_bytes()
    if data[:6] != b"\x7fELF\x01\x01":
        raise SystemExit("expected ELF32 little-endian")
    hdr = struct.unpack_from("<16sHHIIIIIHHHHHH", data, 0)
    _, etype, machine, _, entry, phoff, _, flags, _, phentsize, phnum, _, _, _ = hdr
    root = pathlib.Path(args.out)
    root.mkdir(parents=True, exist_ok=True)
    manifest = {"elf_type": etype, "machine": machine, "entry": entry, "flags": flags, "segments": []}
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz, pflags, align = struct.unpack_from(
            "<IIIIIIII", data, phoff + i * phentsize)
        if ptype != 1:
            continue
        name = f"load{i}_v{vaddr:08x}.bin"
        (root / name).write_bytes(data[off:off + filesz])
        manifest["segments"].append({
            "index": i, "file": name, "file_offset": off, "vaddr": vaddr,
            "paddr": paddr, "filesz": filesz, "memsz": memsz,
            "flags": pflags, "align": align,
        })
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2))
    print(json.dumps(manifest, indent=2))


if __name__ == "__main__":
    main()
