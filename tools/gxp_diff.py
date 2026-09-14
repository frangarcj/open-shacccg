#!/usr/bin/env python3
import argparse

def main():
    ap=argparse.ArgumentParser(description='Byte-range diff for two GXP binaries')
    ap.add_argument('a'); ap.add_argument('b'); args=ap.parse_args()
    a=open(args.a,'rb').read(); b=open(args.b,'rb').read(); n=max(len(a),len(b))
    ranges=[]; start=None
    for i in range(n):
        x=a[i] if i<len(a) else None; y=b[i] if i<len(b) else None
        if x != y and start is None: start=i
        if x == y and start is not None: ranges.append((start,i)); start=None
    if start is not None: ranges.append((start,n))
    print(f'{len(a)} bytes vs {len(b)} bytes; {len(ranges)} changed ranges')
    for s,e in ranges[:200]: print(f'0x{s:06x}-0x{e-1:06x} ({e-s} bytes)')
if __name__=='__main__': main()
