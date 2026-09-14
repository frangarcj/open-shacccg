#!/usr/bin/env python3
"""Minimal dependency-free inspector for a Vita/ARM ELF oracle module."""
import argparse, struct

def cstrs(data, min_len=6):
    out=[]; start=None
    for i,b in enumerate(data+b'\0'):
        if 32 <= b < 127:
            if start is None: start=i
        else:
            if start is not None and i-start >= min_len:
                out.append(data[start:i].decode('ascii','replace'))
            start=None
    return out

def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('elf')
    ap.add_argument('--strings', action='store_true')
    args=ap.parse_args()
    data=open(args.elf,'rb').read()
    if data[:4] != b'\x7fELF' or data[4] != 1 or data[5] != 1:
        raise SystemExit('expected ELF32 little-endian')
    e=struct.unpack_from('<16sHHIIIIIHHHHHH', data, 0)
    _,etype,machine,version,entry,phoff,shoff,flags,ehsize,phentsize,phnum,shentsize,shnum,shstr=e
    print(f'type=0x{etype:04x} machine={machine} entry=0x{entry:08x} flags=0x{flags:08x}')
    print(f'phoff={phoff} phnum={phnum} phentsize={phentsize} sections={shnum}')
    for i in range(phnum):
        p=struct.unpack_from('<IIIIIIII', data, phoff+i*phentsize)
        ptype,off,vaddr,paddr,filesz,memsz,pflags,align=p
        print(f'PH{i}: type=0x{ptype:x} off=0x{off:x} vaddr=0x{vaddr:08x} filesz=0x{filesz:x} memsz=0x{memsz:x} flags=0x{pflags:x} align=0x{align:x}')
    if args.strings:
        needles=('SceShacc','USSE','sce_vp_psp2','sce_fp_psp2','Compiler','GXP')
        for s in cstrs(data):
            if any(n in s for n in needles): print(s)
if __name__=='__main__': main()
