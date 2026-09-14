#!/usr/bin/env python3
"""Parse module info and import/export NIDs from a Vita ET_SCE_RELEXEC.

The data structures are public Vita reverse-engineering/toolchain formats. This
script is host-side research tooling only; it does not copy implementation from
the target module.
"""
import argparse
import json
import pathlib
import struct

MODULE_INFO = struct.Struct('<HH27sBIIIIII3IIIIIII')
# size, libver bytes, attr, nfunc, nvar, ntls, nid, name, nids, entries
EXPORTS = struct.Struct('<H2sHHIIIIII')
IMPORT_2XX = struct.Struct('<HHHHHHIIIIIIIIII')  # 0x34
IMPORT_3XX = struct.Struct('<HHHHHHIIIIII')      # 0x24

KNOWN_NIDS = {
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


def read_cstr(data, off, limit=512):
    if off < 0 or off >= len(data):
        return None
    end = data.find(b'\0', off, min(len(data), off + limit))
    if end < 0:
        return None
    return data[off:end].decode('ascii', 'replace')


def u32_array(data, off, count):
    if off < 0 or off + count * 4 > len(data):
        return []
    return list(struct.unpack_from('<' + 'I' * count, data, off))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--json', action='store_true')
    args = ap.parse_args()
    raw = pathlib.Path(args.elf).read_bytes()
    if raw[:6] != b'\x7fELF\x01\x01':
        raise SystemExit('expected ELF32 little-endian')
    eh = struct.unpack_from('<16sHHIIIIIHHHHHH', raw, 0)
    _, etype, machine, _, entry, phoff, _, flags, _, phentsz, phnum, _, _, _ = eh
    loads = []
    for i in range(phnum):
        ptype, off, vaddr, paddr, filesz, memsz, pflags, align = struct.unpack_from('<IIIIIIII', raw, phoff + i * phentsz)
        if ptype == 1:
            loads.append({'index': i, 'off': off, 'vaddr': vaddr, 'filesz': filesz, 'memsz': memsz})
    if not loads:
        raise SystemExit('no PT_LOAD segments')
    text = loads[0]
    seg = raw[text['off']:text['off'] + text['filesz']]

    # Vita modules place the module name at +4 inside SceModuleInfo. Searching
    # makes this robust for modules whose e_entry encoding differs by firmware.
    candidates = []
    for marker in (b'SceShaccCg\0',):
        pos = seg.find(marker)
        while pos >= 4:
            mi_off = pos - 4
            if mi_off + MODULE_INFO.size <= len(seg):
                candidates.append(mi_off)
            pos = seg.find(marker, pos + 1)
    if not candidates:
        # For this module e_entry is also a segment-relative module-info offset.
        if entry + MODULE_INFO.size <= len(seg):
            candidates.append(entry)
        else:
            raise SystemExit('module info not found')

    mi_off = candidates[0]
    fields = MODULE_INFO.unpack_from(seg, mi_off)
    (attr, ver, name_raw, mtype, gp, exp_top, exp_btm, imp_top, imp_btm,
     module_nid, unk0, unk1, unk2, start, stop, exidx_top, exidx_btm,
     extab_top, extab_btm) = fields
    name = name_raw.split(b'\0', 1)[0].decode('ascii', 'replace')

    def va_to_seg_off(va):
        return va - text['vaddr']

    exports = []
    cur = exp_top
    while cur < exp_btm and cur + 2 <= len(seg):
        size = struct.unpack_from('<H', seg, cur)[0]
        if size == 0 or cur + size > len(seg):
            break
        if size >= EXPORTS.size:
            vals = EXPORTS.unpack_from(seg, cur)
            sz, libver, eattr, nfunc, nvar, ntls, libnid, libname_va, nid_va, entry_va = vals
            libname = read_cstr(seg, va_to_seg_off(libname_va)) if libname_va else None
            nids = u32_array(seg, va_to_seg_off(nid_va), nfunc + nvar + ntls) if nid_va else []
            entries = u32_array(seg, va_to_seg_off(entry_va), nfunc + nvar + ntls) if entry_va else []
            exports.append({'offset': cur, 'size': sz, 'library': libname, 'library_nid': libnid,
                            'attribute': eattr, 'version_raw': libver.hex(), 'num_functions': nfunc,
                            'num_vars': nvar, 'num_tls_vars': ntls,
                            'function_nids': nids[:nfunc], 'function_entries': entries[:nfunc]})
        cur += size

    imports = []
    cur = imp_top
    while cur < imp_btm and cur + 2 <= len(seg):
        size = struct.unpack_from('<H', seg, cur)[0]
        if size == 0 or cur + size > len(seg):
            break
        if size == IMPORT_2XX.size:
            vals = IMPORT_2XX.unpack_from(seg, cur)
            (sz, libver, iattr, nfunc, nvar, ntls, reserved1, libnid, libname_va, reserved2,
             fnid_va, fent_va, vnid_va, vent_va, tnid_va, tent_va) = vals
        elif size == IMPORT_3XX.size:
            vals = IMPORT_3XX.unpack_from(seg, cur)
            (sz, libver, iattr, nfunc, nvar, ntls, libnid, libname_va,
             fnid_va, fent_va, vnid_va, vent_va) = vals
            tnid_va = tent_va = 0
        else:
            imports.append({'offset': cur, 'size': size, 'error': 'unknown import table size'})
            cur += size
            continue
        libname = read_cstr(seg, va_to_seg_off(libname_va)) if libname_va else None
        fnids = u32_array(seg, va_to_seg_off(fnid_va), nfunc) if fnid_va else []
        fentries = u32_array(seg, va_to_seg_off(fent_va), nfunc) if fent_va else []
        vnids = u32_array(seg, va_to_seg_off(vnid_va), nvar) if vnid_va else []
        ventries = u32_array(seg, va_to_seg_off(vent_va), nvar) if vent_va else []
        imports.append({'offset': cur, 'size': size, 'library': libname, 'library_nid': libnid,
                        'attribute': iattr, 'version': libver, 'num_functions': nfunc,
                        'num_vars': nvar, 'num_tls_vars': ntls,
                        'function_nids': fnids, 'function_names': [KNOWN_NIDS.get(x) for x in fnids], 'function_entries': fentries,
                        'variable_nids': vnids, 'variable_entries': ventries})
        cur += size

    report = {
        'elf_type': etype, 'machine': machine, 'flags': flags,
        'text_vaddr': text['vaddr'], 'module_info_offset': mi_off,
        'module': {'name': name, 'type': mtype, 'attribute': attr, 'version': ver,
                   'nid': module_nid, 'gp': gp, 'exports_top': exp_top,
                   'exports_bottom': exp_btm, 'imports_top': imp_top,
                   'imports_bottom': imp_btm, 'start': start, 'stop': stop},
        'exports': exports, 'imports': imports,
    }
    if args.json:
        print(json.dumps(report, indent=2))
        return
    print(f"module {name} nid=0x{module_nid:08X} type={mtype} module_info=+0x{mi_off:X}")
    print(f"exports +0x{exp_top:X}..+0x{exp_btm:X}; imports +0x{imp_top:X}..+0x{imp_btm:X}")
    print('\nExports:')
    for lib in exports:
        print(f"  {lib['library'] or '<NONAME>'} nid=0x{lib['library_nid']:08X} funcs={lib['num_functions']}")
        for nid, entry_va in zip(lib['function_nids'], lib['function_entries']):
            print(f"    0x{nid:08X} -> 0x{entry_va:08X}")
    print('\nImports:')
    for lib in imports:
        if 'error' in lib:
            print(f"  +0x{lib['offset']:X}: {lib['error']} size=0x{lib['size']:X}")
            continue
        print(f"  {lib['library'] or '<NONAME>'} nid=0x{lib['library_nid']:08X} funcs={lib['num_functions']} vars={lib['num_vars']}")
        for nid, fname, entry_va in zip(lib['function_nids'], lib['function_names'], lib['function_entries']):
            label = fname or '<unresolved>'
            print(f"    F 0x{nid:08X} {label} stub=0x{entry_va:08X}")
        for nid, entry_va in zip(lib['variable_nids'], lib['variable_entries']):
            print(f"    V 0x{nid:08X} slot=0x{entry_va:08X}")


if __name__ == '__main__':
    main()
