#!/usr/bin/env python3
"""Structural comparison of two GXP programs."""
import argparse
import pathlib
import sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
from gxp_info import parse


def main():
    ap=argparse.ArgumentParser()
    ap.add_argument('a',type=pathlib.Path); ap.add_argument('b',type=pathlib.Path)
    ns=ap.parse_args()
    a=parse(ns.a.read_bytes()); b=parse(ns.b.read_bytes())
    keys=['stage','logical_size','program_flags','buffer_flags','texunit_flags','registers',
          'buffers','compiler_version_raw','literals_count','uniform_buffer_count',
          'dependent_sampler_count','container_count']
    for k in keys:
        if a[k] != b[k]: print(f'{k}:\n  A {a[k]}\n  B {b[k]}')
    print(f"parameters: {len(a['parameters'])} -> {len(b['parameters'])}")
    for side,r in [('A',a),('B',b)]:
        print(f'{side} params:', ', '.join(p['name'] for p in r['parameters']) or '(none)')
    print(f"primary instructions: {a['primary_program']['instruction_count']} -> {b['primary_program']['instruction_count']}")
    aq=[x['qword'] for x in a['primary_program']['instructions']]
    bq=[x['qword'] for x in b['primary_program']['instructions']]
    n=max(len(aq),len(bq))
    for i in range(n):
        x=aq[i] if i<len(aq) else '-'; y=bq[i] if i<len(bq) else '-'
        print(('  ' if x==y else '* ')+f'{i:02}: {x}  {y}')

if __name__=='__main__': main()
