#!/usr/bin/env python3
"""Regenerate fixed backend vectors; --check compares without modifying them."""
from pathlib import Path
import argparse
import hashlib

def generate():
    rows=[]
    sizes=[0,1,2,3,7,8,15,16,31,32,55,56,57,63,64,65,111,112,113,127,128,129,255,256,257,4095,4096,4097,65535,65536,65537,1048576,1048592]
    for algorithm in ['sha1','sha256','sha384','sha512']:
        for size in sizes:
            for pattern in [0,1]:
                payload=bytes(size) if not pattern else bytes((i*73+19)&255 for i in range(size))
                expected=hashlib.new(algorithm,payload).digest()
                for offset in [0,7]:
                    rows.append('{MBEDTLS_MD_'+algorithm.upper()+','+','.join(map(str,[size,pattern,offset,len(expected)]))+', {'+','.join(map(str,expected))+'}}')
    assert len(rows)==528
    return ('static const Vector vectors[] = {\n'+',\n'.join(rows)+'\n};\n').encode('utf-8')

def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check',action='store_true')
    parser.add_argument('--output',type=Path,default=Path(__file__).resolve().parents[1]/'tests/fixtures/platform-digest-vectors.h')
    args=parser.parse_args();expected=generate()
    if args.check:
        if args.output.read_bytes()!=expected:raise SystemExit('platform digest vectors differ from independent Python hashlib generation')
        print('528 fixed platform digest vectors match Python hashlib')
    else:
        args.output.write_bytes(expected)

if __name__=='__main__':main()
