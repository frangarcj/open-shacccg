#!/usr/bin/env python3
"""Local-only ARM oracle runner for Sony's SceShaccCg module.

This file contains no Sony code and does not redistribute the proprietary ELF.
It expects the user to provide their own module path at runtime.

The runner uses Unicorn when installed (`pip install unicorn`). It loads the
module's PT_LOAD segments at their linked virtual addresses, replaces Vita
import stubs with ARM SVC trampolines in memory, and provides enough host-side
SceLibKernel/SceRtcUser behavior to call the public SceShaccCg exports.

Current target: GetVersionString + InitializeCompileOptions. CompileProgram
support includes allocator/open-file callback bridges and is intentionally
written so the remaining syscall semantics can be filled in incrementally.
"""
from __future__ import annotations

import argparse
import json
import struct
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, Iterable, Optional

try:
    from unicorn import Uc, UcError
    from unicorn import UC_ARCH_ARM, UC_MODE_ARM, UC_MODE_LITTLE_ENDIAN
    from unicorn import UC_HOOK_INTR
    from unicorn.arm_const import (
        UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
        UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
        UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11,
        UC_ARM_REG_R12, UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC,
        UC_ARM_REG_CPSR,
    )
except ImportError as exc:  # pragma: no cover - dependency is host optional
    Uc = None
    UcError = Exception
    _UNICORN_IMPORT_ERROR = exc

PAGE = 0x1000
STACK_BASE = 0x9F000000
STACK_SIZE = 0x00800000
HEAP_BASE = 0x90000000
HEAP_SIZE = 0x04000000
TRAMP_BASE = 0x8F000000
TRAMP_SIZE = 0x00010000
RETURN_ADDR = TRAMP_BASE + 0xF000

# Custom host callback trap IDs. Import trap IDs are read from trap_map.json.
TRAP_MALLOC = 0x1000
TRAP_FREE = 0x1001
TRAP_OPEN_FILE = 0x1010
TRAP_RELEASE_FILE = 0x1011

EXPORT_NIDS = {
    0x3B58AFA0: "sceShaccCgInitializeCompileOptions",
    0x66814F35: "sceShaccCgCompileProgram",
    0x6F01D573: "sceShaccCgSetDefaultAllocator",
    0x7F430CCD: "sceShaccCgGetVersionString",
    0x95F57A23: "sceShaccCgReleaseCompiler",
    0xA8C2C1C8: "sceShaccCgInitializeCallbackList",
    0xAA82EF0C: "sceShaccCgDestroyCompileOutput",
}

# Known imports in the uploaded 1.6.5 module. Unknown NIDs remain visible in
# trace output instead of being silently guessed.
KNOWN_IMPORTS = {
    0x14E9DBD7: "sceClibMemcpy",
    0x244E76D2: "sceKernelDeleteLwMutex",
    0x2F2C6046: "sceClibAbort",
    0x5EA3B6CE: "sceClibVprintf",
    0x614076B7: "sceClibStrchr",
    0x632980D7: "sceClibMemset",
    0x660D1F6D: "sceClibStrncmp",
    0x70CBC2D5: "sceClibStrlcat",
    0x736753C8: "sceClibMemmove",
    0x9CC2BFDF: "sceClibMemcmp",
    0xA2FB4D9D: "sceClibStrcmp",
    0xAC595E68: "sceClibStrnlen",
    0xB110C123: "sceKernelGetProcessTimeWide",
    0xB54C0BE4: "sceClibStrncasecmp",
    0xC458D60A: "sceClibStrncpy",
    0xDA6EC8EF: "sceKernelCreateLwMutex",
    0xFA26BC62: "sceClibPrintf",
    0xFA6BE467: "sceClibVsnprintf",
    0x1282C436: "sceRtcConvertUtcToLocalTime",
    0x23F79274: "sceRtcGetCurrentTick",
    0x3A332F81: "sceRtcSetTime_t",
    0xCD89F464: "sceRtcSetTick",
    0xF2B238E2: "sceRtcGetTick",
}


def align_down(v: int, a: int = PAGE) -> int:
    return v & ~(a - 1)


def align_up(v: int, a: int = PAGE) -> int:
    return (v + a - 1) & ~(a - 1)


def u32(b: bytes, off: int = 0) -> int:
    return struct.unpack_from("<I", b, off)[0]


def p32(v: int) -> bytes:
    return struct.pack("<I", v & 0xFFFFFFFF)


def arm_svc(trap: int) -> bytes:
    if not 0 <= trap <= 0x00FFFFFF:
        raise ValueError("ARM SVC immediate must fit in 24 bits")
    return p32(0xEF000000 | trap)


def arm_bx_lr() -> bytes:
    return p32(0xE12FFF1E)


@dataclass
class LoadSegment:
    file_offset: int
    vaddr: int
    filesz: int
    memsz: int


@dataclass
class Trap:
    trap: int
    function_nid: int
    function_name: Optional[str]
    stub_va: int


class Elf32:
    def __init__(self, data: bytes):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise ValueError("expected little-endian ELF32")
        self.data = data
        self.entry = u32(data, 24)
        phoff = u32(data, 28)
        phentsize = struct.unpack_from("<H", data, 42)[0]
        phnum = struct.unpack_from("<H", data, 44)[0]
        self.segments: list[LoadSegment] = []
        for i in range(phnum):
            off = phoff + i * phentsize
            p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz, _flags, _align = \
                struct.unpack_from("<IIIIIIII", data, off)
            if p_type == 1:  # PT_LOAD
                self.segments.append(LoadSegment(p_offset, p_vaddr, p_filesz, p_memsz))

    def read_va(self, va: int, size: int) -> bytes:
        for s in self.segments:
            if s.vaddr <= va and va + size <= s.vaddr + s.filesz:
                off = s.file_offset + (va - s.vaddr)
                return self.data[off:off + size]
        raise ValueError(f"VA 0x{va:08X} is not file-backed")


class BumpHeap:
    def __init__(self, start: int, size: int):
        self.start = start
        self.end = start + size
        self.cursor = start
        self.allocations: Dict[int, int] = {}

    def malloc(self, size: int, alignment: int = 16) -> int:
        size = max(int(size), 1)
        p = align_up(self.cursor, alignment)
        end = p + size
        if end > self.end:
            return 0
        self.cursor = end
        self.allocations[p] = size
        return p

    def free(self, ptr: int) -> None:
        self.allocations.pop(ptr, None)


class Oracle:
    def __init__(self, elf_path: Path, imports_path: Path, trap_map_path: Path, trace: bool = False):
        if Uc is None:
            raise RuntimeError(
                "Unicorn is required for execution. Install it on the host with `pip install unicorn`. "
                f"Import error: {_UNICORN_IMPORT_ERROR}"
            )
        self.elf_path = elf_path
        self.elf = Elf32(elf_path.read_bytes())
        self.imports = json.loads(imports_path.read_text())
        trap_json = json.loads(trap_map_path.read_text())
        self.traps: Dict[int, Trap] = {}
        for t in trap_json["traps"]:
            tr = Trap(
                trap=int(t["trap"]),
                function_nid=int(t["function_nid"]),
                function_name=t.get("function_name") or KNOWN_IMPORTS.get(int(t["function_nid"])),
                stub_va=int(t["stub_va"]),
            )
            self.traps[tr.trap] = tr
        self.exports = self._parse_exports()
        self.trace = trace
        self.heap = BumpHeap(HEAP_BASE, HEAP_SIZE)
        self.source_file_ptr = 0
        self.source_name_ptr = 0
        self.source_text_ptr = 0
        self.source_size = 0
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM | UC_MODE_LITTLE_ENDIAN)
        self._map_image()
        self._install_trampolines()
        self.uc.hook_add(UC_HOOK_INTR, self._on_interrupt)

    def _parse_exports(self) -> Dict[int, int]:
        out: Dict[int, int] = {}
        for lib in self.imports.get("exports", []):
            # vita_imports.py uses `library` plus parallel NID/entry arrays.
            if lib.get("library") != "SceShaccCg":
                continue
            for nid, address in zip(lib.get("function_nids", []), lib.get("function_entries", [])):
                out[int(nid)] = int(address)
        missing = set(EXPORT_NIDS) - set(out)
        if missing:
            raise ValueError("missing expected exports: " + ", ".join(f"0x{x:08X}" for x in sorted(missing)))
        return out

    def _map(self, address: int, size: int) -> None:
        self.uc.mem_map(align_down(address), align_up((address - align_down(address)) + size))

    def _map_image(self) -> None:
        mapped: list[tuple[int, int]] = []
        for seg in self.elf.segments:
            lo = align_down(seg.vaddr)
            hi = align_up(seg.vaddr + seg.memsz)
            # PT_LOADs in this module do not overlap. Keep this explicit so a
            # future oracle version fails loudly if that assumption changes.
            for a, b in mapped:
                if lo < b and a < hi:
                    raise ValueError("overlapping PT_LOAD mapping is not supported yet")
            self.uc.mem_map(lo, hi - lo)
            self.uc.mem_write(seg.vaddr, self.elf.data[seg.file_offset:seg.file_offset + seg.filesz])
            mapped.append((lo, hi))

        self.uc.mem_map(STACK_BASE, STACK_SIZE)
        self.uc.mem_map(HEAP_BASE, HEAP_SIZE)
        self.uc.mem_map(TRAMP_BASE, TRAMP_SIZE)
        self.uc.reg_write(UC_ARM_REG_SP, STACK_BASE + STACK_SIZE - 0x1000)

        # Patch imports in memory only. Nothing is written back to the Sony ELF.
        for t in self.traps.values():
            self.uc.mem_write(t.stub_va, arm_svc(t.trap) + arm_bx_lr())

    def _trampoline(self, address: int, trap: int) -> int:
        self.uc.mem_write(address, arm_svc(trap) + arm_bx_lr())
        return address  # even address: callback enters ARM state through BLX

    def _install_trampolines(self) -> None:
        self.malloc_cb = self._trampoline(TRAMP_BASE + 0x000, TRAP_MALLOC)
        self.free_cb = self._trampoline(TRAMP_BASE + 0x010, TRAP_FREE)
        self.open_file_cb = self._trampoline(TRAMP_BASE + 0x020, TRAP_OPEN_FILE)
        self.release_file_cb = self._trampoline(TRAMP_BASE + 0x030, TRAP_RELEASE_FILE)
        # Stop address for direct host->guest calls. emu_start() exits when the
        # PC reaches this address, so no SVC is needed here.
        self.uc.mem_write(RETURN_ADDR, arm_bx_lr())

    def read(self, addr: int, size: int) -> bytes:
        return bytes(self.uc.mem_read(addr, size))

    def write(self, addr: int, data: bytes) -> None:
        self.uc.mem_write(addr, data)

    def read_u32(self, addr: int) -> int:
        return u32(self.read(addr, 4))

    def write_u32(self, addr: int, value: int) -> None:
        self.write(addr, p32(value))

    def read_cstr(self, addr: int, limit: int = 1 << 20) -> str:
        if addr == 0:
            return ""
        chunks = bytearray()
        for _ in range(limit):
            c = self.read(addr + len(chunks), 1)[0]
            if c == 0:
                return chunks.decode("utf-8", "replace")
            chunks.append(c)
        raise RuntimeError("unterminated guest string")

    def put_bytes(self, data: bytes, alignment: int = 4) -> int:
        ptr = self.heap.malloc(len(data), alignment)
        if not ptr:
            raise MemoryError("oracle guest heap exhausted")
        self.write(ptr, data)
        return ptr

    def put_cstr(self, text: str) -> int:
        return self.put_bytes(text.encode() + b"\0", 1)

    def call(self, addr: int, *args: int, max_insn: int = 50_000_000) -> int:
        regs = [UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3]
        for i, r in enumerate(regs):
            self.uc.reg_write(r, int(args[i]) if i < len(args) else 0)
        # Additional AAPCS words are passed on the guest stack.
        sp = STACK_BASE + STACK_SIZE - 0x1000
        extra = args[4:]
        if extra:
            sp -= 4 * len(extra)
            self.write(sp, b"".join(p32(int(x)) for x in extra))
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN_ADDR)
        cpsr = self.uc.reg_read(UC_ARM_REG_CPSR)
        if addr & 1:
            cpsr |= 0x20
        else:
            cpsr &= ~0x20
        self.uc.reg_write(UC_ARM_REG_CPSR, cpsr)
        begin = addr & ~1
        self.uc.emu_start(begin, RETURN_ADDR, count=max_insn)
        return self.uc.reg_read(UC_ARM_REG_R0) & 0xFFFFFFFF

    def call_export(self, nid: int, *args: int) -> int:
        return self.call(self.exports[nid], *args)

    def _on_interrupt(self, uc, intno, _user_data):
        pc = uc.reg_read(UC_ARM_REG_PC)
        # ARM SVC advances PC by four. Decode the immediate rather than relying
        # on Unicorn's architecture-specific interrupt number.
        insn = u32(bytes(uc.mem_read(pc - 4, 4)))
        if (insn & 0x0F000000) != 0x0F000000:
            raise RuntimeError(f"unexpected ARM interrupt {intno} at 0x{pc:08X}")
        trap = insn & 0x00FFFFFF
        self._dispatch(trap)

    def _ret(self, value: int) -> None:
        self.uc.reg_write(UC_ARM_REG_R0, int(value) & 0xFFFFFFFF)

    def _args(self) -> tuple[int, int, int, int]:
        return tuple(self.uc.reg_read(r) & 0xFFFFFFFF for r in
                     (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3))

    def _dispatch(self, trap_id: int) -> None:
        a0, a1, a2, a3 = self._args()
        if trap_id == TRAP_MALLOC:
            ptr = self.heap.malloc(a0)
            if self.trace:
                print(f"[oracle] malloc({a0}) -> 0x{ptr:08X}", file=sys.stderr)
            self._ret(ptr)
            return
        if trap_id == TRAP_FREE:
            self.heap.free(a0)
            self._ret(0)
            return
        if trap_id == TRAP_OPEN_FILE:
            # SceShaccCgSourceFile { fileName, text, size }
            if self.trace:
                print(f"[oracle] openFile({self.read_cstr(a0)!r}) -> 0x{self.source_file_ptr:08X}", file=sys.stderr)
            self._ret(self.source_file_ptr)
            return
        if trap_id == TRAP_RELEASE_FILE:
            self._ret(0)
            return

        tr = self.traps.get(trap_id)
        if tr is None:
            raise RuntimeError(f"unregistered SVC trap {trap_id}")
        name = tr.function_name or KNOWN_IMPORTS.get(tr.function_nid)
        if self.trace:
            print(f"[oracle] import trap={trap_id} nid=0x{tr.function_nid:08X} {name or '<unknown>'} "
                  f"args={a0:08x},{a1:08x},{a2:08x},{a3:08x}", file=sys.stderr)
        self._dispatch_import(tr.function_nid, name, a0, a1, a2, a3)

    def _dispatch_import(self, nid: int, name: Optional[str], a0: int, a1: int, a2: int, a3: int) -> None:
        if name == "sceClibMemcpy":
            self.write(a0, self.read(a1, a2)); self._ret(a0); return
        if name == "sceClibMemmove":
            data = self.read(a1, a2); self.write(a0, data); self._ret(a0); return
        if name == "sceClibMemset":
            self.write(a0, bytes([a1 & 0xFF]) * a2); self._ret(a0); return
        if name == "sceClibMemcmp":
            x, y = self.read(a0, a2), self.read(a1, a2)
            self._ret(0 if x == y else (-1 if x < y else 1)); return
        if name in ("sceClibStrcmp", "sceClibStrncmp", "sceClibStrncasecmp"):
            x, y = self.read_cstr(a0), self.read_cstr(a1)
            if name == "sceClibStrncmp": x, y = x[:a2], y[:a2]
            if name == "sceClibStrncasecmp": x, y = x[:a2].lower(), y[:a2].lower()
            self._ret(0 if x == y else (-1 if x < y else 1)); return
        if name == "sceClibStrnlen":
            self._ret(min(len(self.read_cstr(a0, max(a1, 1))), a1)); return
        if name == "sceClibStrchr":
            s = self.read_cstr(a0)
            ch = chr(a1 & 0xFF)
            idx = s.find(ch)
            self._ret(0 if idx < 0 else a0 + idx); return
        if name == "sceClibStrncpy":
            src = self.read_cstr(a1).encode()
            out = src[:a2]
            if len(out) < a2: out += b"\0" * (a2 - len(out))
            self.write(a0, out); self._ret(a0); return
        if name == "sceClibStrlcat":
            dst, src = self.read_cstr(a0), self.read_cstr(a1)
            total = len(dst) + len(src)
            if a2:
                merged = (dst + src).encode()[:max(a2 - 1, 0)] + b"\0"
                self.write(a0, merged)
            self._ret(total); return
        if name == "sceKernelCreateLwMutex":
            # The compiler oracle is single-threaded. A zeroed work area plus
            # success is sufficient until lock/unlock imports are observed.
            if a0: self.write(a0, b"\0" * 0x20)
            self._ret(0); return
        if name == "sceKernelDeleteLwMutex":
            self._ret(0); return
        if name == "sceKernelGetProcessTimeWide":
            # Deterministic monotonic-enough synthetic process time.
            self._ret(0); self.uc.reg_write(UC_ARM_REG_R1, 0); return
        if name in ("sceRtcGetCurrentTick", "sceRtcConvertUtcToLocalTime", "sceRtcSetTime_t", "sceRtcSetTick", "sceRtcGetTick"):
            # Deterministic synthetic time. RTC structures are only relevant for
            # file-date bookkeeping; zero is acceptable for first-pass oracle.
            if a0: self.write(a0, b"\0" * 16)
            if a1 and name in ("sceRtcConvertUtcToLocalTime",): self.write(a1, b"\0" * 8)
            self._ret(0); return
        if name == "sceClibAbort":
            raise RuntimeError("guest called sceClibAbort")
        if name in ("sceClibPrintf", "sceClibVprintf"):
            # Logging only; exact formatting is not required for successful codegen.
            fmt = self.read_cstr(a0) if a0 else ""
            if self.trace: print(f"[guest printf] {fmt}", file=sys.stderr)
            self._ret(len(fmt)); return
        if name == "sceClibVsnprintf":
            # Minimal safe fallback. This gets upgraded if compile traces prove
            # formatted strings affect semantic output rather than diagnostics.
            fmt = self.read_cstr(a2) if a2 else ""
            data = fmt.encode()[:max(a1 - 1, 0)] if a1 else b""
            if a1:
                self.write(a0, data + b"\0")
            self._ret(len(fmt)); return

        raise RuntimeError(f"unimplemented import NID 0x{nid:08X} ({name or 'unknown'})")

    def install_allocator(self) -> int:
        return self.call_export(0x6F01D573, self.malloc_cb, self.free_cb)

    def get_version(self) -> str:
        ptr = self.call_export(0x7F430CCD)
        return self.read_cstr(ptr)

    def initialize_options(self) -> tuple[int, bytes]:
        ptr = self.heap.malloc(0x68)
        self.write(ptr, b"\xA5" * 0x68)
        rc = self.call_export(0x3B58AFA0, ptr)
        return rc, self.read(ptr, 0x68)

    def compile_source(self, source: str, stage: str = "fragment", entry: str = "main") -> tuple[int, bytes, list[dict]]:
        if self.install_allocator() != 0:
            raise RuntimeError("sceShaccCgSetDefaultAllocator failed")

        # Build source file object in guest memory.
        self.source_name_ptr = self.put_cstr("<oracle>")
        self.source_text_ptr = self.put_bytes(source.encode() + b"\0", 1)
        self.source_size = len(source.encode())
        self.source_file_ptr = self.heap.malloc(12)
        self.write(self.source_file_ptr, p32(self.source_name_ptr) + p32(self.source_text_ptr) + p32(self.source_size))

        options = self.heap.malloc(0x68)
        self.write(options, b"\0" * 0x68)
        rc = self.call_export(0x3B58AFA0, options)
        if rc != 0:
            raise RuntimeError(f"InitializeCompileOptions returned {rc:#x}")
        self.write_u32(options + 0x00, self.source_name_ptr)
        self.write_u32(options + 0x04, 1 if stage == "fragment" else 0)
        self.write_u32(options + 0x08, self.put_cstr(entry))
        self.write_u32(options + 0x30, 1)  # useFx, as vitaShaRK does

        callbacks = self.heap.malloc(0x18)
        self.write(callbacks, b"\0" * 0x18)
        self.call_export(0xA8C2C1C8, callbacks, 1)  # SCE_SHACCCG_TRIVIAL
        self.write_u32(callbacks + 0x00, self.open_file_cb)
        self.write_u32(callbacks + 0x04, self.release_file_cb)

        output = self.call_export(0x66814F35, options, callbacks, 0, max_insn=200_000_000)
        if not output:
            return 0, b"", [{"level": 2, "code": 0, "message": "CompileProgram returned NULL"}]
        program_ptr = self.read_u32(output + 0x00)
        program_size = self.read_u32(output + 0x04)
        diag_count = self.read_u32(output + 0x08)
        diag_ptr = self.read_u32(output + 0x0C)
        gxp = self.read(program_ptr, program_size) if program_ptr and program_size else b""
        diags: list[dict] = []
        for i in range(diag_count):
            d = diag_ptr + 16 * i
            level = self.read_u32(d + 0)
            code = self.read_u32(d + 4)
            loc = self.read_u32(d + 8)
            msg = self.read_u32(d + 12)
            item = {"level": level, "code": code, "message": self.read_cstr(msg) if msg else ""}
            if loc:
                item["line"] = self.read_u32(loc + 4)
                item["column"] = self.read_u32(loc + 8)
            diags.append(item)
        return output, gxp, diags


def format_options(raw: bytes) -> dict:
    words = list(struct.unpack("<26I", raw))
    keys = [
        "mainSourceFile", "targetProfile", "entryFunctionName", "searchPathCount",
        "searchPaths", "macroDefinitionCount", "macroDefinitions", "includeFileCount",
        "includeFiles", "suppressedWarningsCount", "suppressedWarnings", "locale",
        "useFx", "noStdlib", "optimizationLevel", "useFastmath", "useFastprecision",
        "useFastint", "field_48", "warningsAsErrors", "performanceWarnings", "warningLevel",
        "pedantic", "pedanticError", "field_60", "field_64",
    ]
    return dict(zip(keys, words))


def main(argv: Optional[Iterable[str]] = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("elf", type=Path)
    ap.add_argument("--imports", type=Path, required=True)
    ap.add_argument("--traps", type=Path, required=True)
    ap.add_argument("--trace", action="store_true")
    ap.add_argument("--probe", choices=("version", "options", "compile"), default="version")
    ap.add_argument("--source", type=Path)
    ap.add_argument("--stage", choices=("vertex", "fragment"), default="fragment")
    ap.add_argument("--out", type=Path)
    ns = ap.parse_args(argv)

    oracle = Oracle(ns.elf, ns.imports, ns.traps, trace=ns.trace)
    if ns.probe == "version":
        print(oracle.get_version())
        return 0
    if ns.probe == "options":
        rc, raw = oracle.initialize_options()
        print(json.dumps({"rc": rc, "raw_hex": raw.hex(), "fields": format_options(raw)}, indent=2))
        return 0

    if not ns.source:
        ap.error("--source is required with --probe compile")
    output, gxp, diags = oracle.compile_source(ns.source.read_text(), ns.stage)
    print(json.dumps({"output": output, "gxp_size": len(gxp), "diagnostics": diags}, indent=2))
    if ns.out and gxp:
        ns.out.write_bytes(gxp)
    if output:
        oracle.call_export(0xAA82EF0C, output)
    oracle.call_export(0x95F57A23)
    return 0 if gxp else 2


if __name__ == "__main__":
    raise SystemExit(main())
