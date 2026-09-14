#!/usr/bin/env python3
"""Structured, dependency-free inspector for Vita GXP program images.

The parser uses the layout independently validated by the C++ GXP reader and
known-good public shader binaries. It is intended for differential research and
never mutates the input.
"""
import argparse
import hashlib
import json
import pathlib
import struct

HEADER_SIZE = 0x9C
PARAM_SIZE = 0x10
INSTR_SIZE = 8


def u8(d, o): return d[o]
def u16(d, o): return struct.unpack_from('<H', d, o)[0]
def u32(d, o): return struct.unpack_from('<I', d, o)[0]
def i32(d, o): return struct.unpack_from('<i', d, o)[0]


def rel(field, value): return field + value


def cstr(d, off, limit):
    end = d.find(b'\0', off, limit)
    if off < 0 or off >= limit or end < 0:
        raise ValueError(f'invalid string at 0x{off:x}')
    return d[off:end].decode('utf-8', 'replace')


def instruction_rows(d, off, count):
    rows=[]
    for i in range(count):
        lo, hi = struct.unpack_from('<II', d, off + i*8)
        rows.append({'index': i, 'offset': off+i*8,
                     'lo': f'0x{lo:08x}', 'hi': f'0x{hi:08x}',
                     'qword': f'0x{hi:08x}{lo:08x}'})
    return rows


def parse(d):
    if len(d) < HEADER_SIZE: raise ValueError('file is smaller than GXP header')
    if d[:4] != b'GXP\0': raise ValueError('invalid GXP magic')
    logical=u32(d,0x08)
    if logical < HEADER_SIZE or logical > len(d):
        raise ValueError(f'invalid logical size 0x{logical:x} for {len(d)}-byte buffer')

    params_count=u32(d,0x24)
    params_off=rel(0x28,u32(d,0x28))
    primary_count=u32(d,0x3c)
    primary_off=rel(0x40,u32(d,0x40))
    secondary_count=u32(d,0x44)
    secondary_off=rel(0x48,u32(d,0x48))
    varyings_off=rel(0x2c,u32(d,0x2c))

    for off,size,name in [
        (params_off, params_count*PARAM_SIZE, 'parameters'),
        (primary_off, primary_count*INSTR_SIZE, 'primary program'),
        (secondary_off, secondary_count*INSTR_SIZE, 'secondary program'),
        (varyings_off, 32, 'varyings'),
    ]:
        if size and (off < 0 or off+size > logical):
            raise ValueError(f'{name} range 0x{off:x}..0x{off+size:x} is outside logical image')

    params=[]
    for i in range(params_count):
        off=params_off+i*PARAM_SIZE
        name_off=off+i32(d,off)
        packed_type=u8(d,off+4); packed_shape=u8(d,off+5)
        params.append({
            'index':i, 'offset':off, 'name':cstr(d,name_off,logical),
            'name_offset':name_off,
            'category':packed_type & 0xf, 'type':packed_type >> 4,
            'component_count':packed_shape & 0xf, 'container_index':packed_shape >> 4,
            'semantic':u8(d,off+6), 'semantic_index':u8(d,off+7),
            'array_size':u32(d,off+8), 'resource_index':u32(d,off+12),
        })

    flags=u32(d,0x14)
    return {
        'size_on_disk': len(d), 'logical_size': logical,
        'padding_bytes': len(d)-logical,
        'version': {'major':u8(d,4),'minor':u8(d,5),'sdk':u16(d,6)},
        'guids': {'binary':f'0x{u32(d,0x0c):08x}','source':f'0x{u32(d,0x10):08x}'},
        'stage':'fragment' if flags & 1 else 'vertex',
        'program_flags':f'0x{flags:08x}',
        'buffer_flags':f'0x{u32(d,0x18):08x}',
        'texunit_flags':[f'0x{u32(d,0x1c):08x}',f'0x{u32(d,0x20):08x}'],
        'registers':{
            'primary':u16(d,0x30), 'secondary':u16(d,0x32),
            'temp1':u32(d,0x34), 'temp2':u16(d,0x38),
            'primary_phase_count':u16(d,0x3a),
        },
        'parameters': params,
        'varyings': {'offset': varyings_off, 'size':32,
                     'hex':d[varyings_off:varyings_off+32].hex()},
        'primary_program': {'offset':primary_off,'instruction_count':primary_count,
                            'instructions':instruction_rows(d,primary_off,primary_count)},
        'secondary_program': {'offset':secondary_off,'instruction_count':secondary_count,
                              'instructions':instruction_rows(d,secondary_off,secondary_count)},
        'buffers':{
            'scratch':u32(d,0x50),'thread':u32(d,0x54),'literal':u32(d,0x58),
            'data':u32(d,0x5c),'texture':u32(d,0x60),'default_uniform':u32(d,0x64),
        },
        'compiler_version_raw':f'0x{u32(d,0x6c):08x}',
        'literals_count':u32(d,0x70),
        'uniform_buffer_count':u32(d,0x78),
        'dependent_sampler_count':u32(d,0x80),
        'texture_buffer_dependent_sampler_count':u32(d,0x88),
        'container_count':u32(d,0x90),
    }


def main():
    ap=argparse.ArgumentParser(description='Inspect a Vita GXP program image')
    ap.add_argument('gxp',type=pathlib.Path)
    ap.add_argument('--json',action='store_true')
    ns=ap.parse_args()
    data=ns.gxp.read_bytes()
    r=parse(data)
    r['file']=str(ns.gxp); r['sha256']=hashlib.sha256(data).hexdigest()
    if ns.json:
        print(json.dumps(r,indent=2)); return
    print(f"{ns.gxp}: {r['stage']} GXP v{r['version']['major']}.{r['version']['minor']}, "
          f"logical={r['logical_size']} bytes disk={r['size_on_disk']} bytes")
    print(f"flags={r['program_flags']} compiler={r['compiler_version_raw']} "
          f"regs PA={r['registers']['primary']} SA={r['registers']['secondary']}")
    print(f"primary: {r['primary_program']['instruction_count']} instructions @ 0x{r['primary_program']['offset']:x}")
    for ins in r['primary_program']['instructions']:
        print(f"  {ins['index']:02d} +0x{ins['offset']:04x} {ins['qword']}")
    print(f"secondary: {r['secondary_program']['instruction_count']} instructions @ 0x{r['secondary_program']['offset']:x}")
    for ins in r['secondary_program']['instructions']:
        print(f"  {ins['index']:02d} +0x{ins['offset']:04x} {ins['qword']}")
    print(f"parameters: {len(r['parameters'])}")
    for p in r['parameters']:
        print(f"  [{p['index']}] {p['name']} cat={p['category']} type={p['type']} comp={p['component_count']} "
              f"container={p['container_index']} array={p['array_size']} resource={p['resource_index']}")

if __name__=='__main__': main()
