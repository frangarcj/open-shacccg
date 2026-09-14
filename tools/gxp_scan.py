#!/usr/bin/env python3
"""Heuristic GXP research scanner.

This does not assume undocumented GXP structs. It reports reproducible facts
(strings, aligned words, and in-blob offset candidates) that are useful for
clean differential reverse engineering.
"""
import argparse
import hashlib
import json
import pathlib
import struct


def ascii_strings(data, minimum=3):
    result = []
    start = None
    for i, b in enumerate(data + b"\0"):
        if 0x20 <= b < 0x7f:
            if start is None:
                start = i
        else:
            if start is not None and i - start >= minimum:
                result.append((start, data[start:i].decode("ascii", "replace")))
            start = None
    return result


def offset_candidates(data):
    out = []
    for off in range(0, len(data) - 3, 4):
        value = struct.unpack_from("<I", data, off)[0]
        if value < len(data) and value % 4 == 0:
            out.append((off, value))
    return out


def qwords(data, nonzero_only=True):
    out = []
    for off in range(0, len(data) - 7, 8):
        lo, hi = struct.unpack_from("<II", data, off)
        if nonzero_only and lo == 0 and hi == 0:
            continue
        out.append((off, lo, hi))
    return out


def main():
    ap = argparse.ArgumentParser(description="Heuristic scanner for Vita GXP binaries")
    ap.add_argument("gxp")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("--max", type=int, default=80, help="max entries per section")
    args = ap.parse_args()

    path = pathlib.Path(args.gxp)
    data = path.read_bytes()
    report = {
        "file": str(path),
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
        "strings": [{"offset": o, "text": s} for o, s in ascii_strings(data)],
        "offset_candidates": [{"field_offset": o, "target_offset": v} for o, v in offset_candidates(data)],
        "qwords": [{"offset": o, "lo": lo, "hi": hi} for o, lo, hi in qwords(data)],
    }
    if args.json:
        print(json.dumps(report, indent=2))
        return

    print(f"{path}: {len(data)} bytes sha256={report['sha256']}")
    print("\nASCII strings:")
    for row in report["strings"][:args.max]:
        print(f"  0x{row['offset']:06x}  {row['text']}")
    print("\n32-bit values that look like in-blob aligned offsets:")
    for row in report["offset_candidates"][:args.max]:
        print(f"  +0x{row['field_offset']:06x} -> 0x{row['target_offset']:06x}")
    print("\nNon-zero 64-bit aligned words (raw research view):")
    for row in report["qwords"][:args.max]:
        print(f"  0x{row['offset']:06x}: {row['hi']:08x}_{row['lo']:08x}")


if __name__ == "__main__":
    main()
