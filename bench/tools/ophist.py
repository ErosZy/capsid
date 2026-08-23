#!/usr/bin/env python3
"""Opcode-frequency histogram for a .qjsb bundle (G5 deliverable).

Layout mirrors the optimizer's reader
(src/bytecode_optimizer/bytecode_optimizer.cc):
header, atom table, module record, then function records reached by
recursing into cpool object records (BC_TAG_FUNCTION_BYTECODE children).
Every function's code blob is decoded with operand sizes from the vendored
quickjs-opcode.h DEF table.

Usage: ophist.py <bundle.qjsb> [--json]
"""
import json
import re
import sys

DEF_RE = re.compile(r'^\s*DEF\(\s*(\w+),\s*(\d+),\s*(\d+),\s*(\d+),\s*(\w+)\)')

# BCTagEnum, vendor/txiki.js/deps/quickjs/quickjs.c:37657
TAG = {
    'NULL': 1, 'UNDEFINED': 2, 'BOOL_FALSE': 3, 'BOOL_TRUE': 4,
    'INT32': 5, 'FLOAT64': 6, 'STRING': 7, 'OBJECT': 8, 'ARRAY': 9,
    'BIG_INT': 10, 'TEMPLATE_OBJECT': 11, 'FUNCTION_BYTECODE': 12,
    'MODULE': 13, 'TYPED_ARRAY': 14, 'ARRAY_BUFFER': 15,
    'SHARED_ARRAY_BUFFER': 16, 'REGEXP': 17, 'DATE': 18,
    'OBJECT_VALUE': 19, 'OBJECT_REFERENCE': 20, 'MAP': 21, 'SET': 22,
    'SYMBOL': 23,
}


def load_op_table(path):
    sizes, names = {}, []
    with open(path) as f:
        for line in f:
            m = DEF_RE.match(line)
            if m:  # 'invalid' occupies opcode 0 like the C++ enum
                names.append(m.group(1))
                sizes[len(names) - 1] = int(m.group(2))
    return sizes, names


class R:
    def __init__(self, buf):
        self.b = buf
        self.p = 0

    def u8(self):
        v = self.b[self.p]; self.p += 1; return v

    def u16(self):
        v = self.b[self.p] | (self.b[self.p + 1] << 8); self.p += 2; return v

    def u32(self):
        v = 0
        for i in range(4):
            v |= self.b[self.p + i] << (8 * i)
        self.p += 4; return v

    def leb(self):
        x = shift = 0
        while True:
            b = self.b[self.p]; self.p += 1
            x |= (b & 0x7f) << shift
            if not (b & 0x80):
                return x
            shift += 7

    def sleb(self):
        x = shift = 0
        while True:
            b = self.b[self.p]; self.p += 1
            x |= (b & 0x7f) << shift
            shift += 7
            if not (b & 0x80):
                break
        if shift < 32 and (b & 0x40):
            x |= ~0 << shift
        return x

    def string(self):
        len2 = self.leb()
        n = len2 >> 1
        w = len2 & 1
        raw = self.b[self.p:self.p + n * (2 if w else 1)]
        self.p += n * (2 if w else 1)
        if w:
            return raw.decode('utf-16-le', 'replace')
        return raw.decode('utf-8', 'replace')

    def atom(self):
        return self.leb()

    def skip_atom_entry(self, atoms):
        t = self.u8()
        if t == 0:
            atoms.append(('const', self.u32()))
        elif 1 <= t <= 3:
            atoms.append(('str', self.string()))
        else:
            raise ValueError(f"atom type {t}")

    def obj(self, functions, atoms, depth=0):
        """One serialized value record (skip_object_rec)."""
        if depth > 4096:
            raise ValueError("nesting too deep")
        tag = self.u8()
        if tag in (TAG['NULL'], TAG['UNDEFINED'], TAG['BOOL_FALSE'],
                   TAG['BOOL_TRUE']):
            return
        if tag == TAG['INT32']:
            self.sleb()
        elif tag == TAG['FLOAT64']:
            self.p += 8
        elif tag == TAG['STRING']:
            self.string()
        elif tag == TAG['SYMBOL']:
            self.atom()
        elif tag == TAG['OBJECT_REFERENCE']:
            self.leb()
        elif tag == TAG['TYPED_ARRAY']:
            self.u8(); self.leb(); self.leb()
            self.obj(functions, atoms, depth + 1)
        elif tag == TAG['ARRAY_BUFFER']:
            n = self.leb(); self.leb()
            self.p += n
        elif tag == TAG['SHARED_ARRAY_BUFFER']:
            self.leb(); self.leb()
            self.p += 8
        elif tag == TAG['ARRAY'] or tag == TAG['TEMPLATE_OBJECT']:
            n = self.leb()
            for _ in range(n):
                self.obj(functions, atoms, depth + 1)
            if tag == TAG['TEMPLATE_OBJECT']:
                self.obj(functions, atoms, depth + 1)  # raw-array record
        elif tag == TAG['OBJECT'] or tag == TAG['OBJECT_VALUE'] or \
                tag == TAG['DATE']:
            n = self.leb()
            for _ in range(n):
                self.atom()  # key
                self.obj(functions, atoms, depth + 1)
        elif tag == TAG['REGEXP']:
            self.string(); self.string()
        elif tag == TAG['BIG_INT']:
            n = self.leb(); self.p += n
        elif tag == TAG['MAP'] or tag == TAG['SET']:
            n = self.leb()
            for _ in range(n):
                self.obj(functions, atoms, depth + 1)
                if tag == TAG['MAP']:
                    self.obj(functions, atoms, depth + 1)
        elif tag == TAG['FUNCTION_BYTECODE']:
            functions.append(self.fn(functions, atoms, depth + 1))
        else:
            raise ValueError(f"unknown BC tag {tag}")

    def fn(self, functions, atoms, depth):
        """Full function record (skip_function)."""
        flags = self.u16()
        self.u8()  # strict
        self.atom()  # name
        # args/var/defargs/stack/varr/closure/cpool counts
        for _ in range(5):
            self.leb()
        closure_c = self.leb()
        cpool_c = self.leb()
        code_len = self.leb()
        for _ in range(self.leb()):  # vardefs
            self.atom(); self.leb(); self.leb()
            if self.u8() & 0x40:
                self.leb()  # var_ref_idx
        for _ in range(closure_c):  # closure vars
            self.atom(); self.leb(); self.leb()
        for _ in range(cpool_c):  # cpool, recursing into children
            self.obj(functions, atoms, depth)
        code_off = self.p  # code blob follows the cpool
        code = self.b[code_off:code_off + code_len]
        self.p = code_off + code_len
        if flags & (1 << 11):  # debug block
            self.atom()  # filename
            self.leb(); self.leb()  # line, col
            pc2 = self.leb(); self.p += pc2
            src = self.leb(); self.p += src
        return {'name_atom': None, 'code': code, 'flags': flags}


def decode(code, sizes, names):
    counts = {}
    i = 0
    while i < len(code):
        op = code[i]
        size = sizes.get(op)
        if size is None:
            raise ValueError(f"unknown opcode {op} at pc {i}")
        counts[op] = counts.get(op, 0) + 1
        i += size
    return counts


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('-')]
    want_json = '--json' in sys.argv
    if len(args) != 1:
        print("usage: ophist.py <bundle.qjsb> [--json]", file=sys.stderr)
        return 2
    sizes, names = load_op_table(
        'vendor/txiki.js/deps/quickjs/quickjs-opcode.h')
    with open(args[0], 'rb') as f:
        buf = f.read()
    r = R(buf)
    assert buf[0] == 26, f"bad version {buf[0]}"
    r.p = 5  # version + checksum
    atoms = []
    for _ in range(r.leb()):
        r.skip_atom_entry(atoms)
    assert r.u8() == TAG['MODULE'], "not a module"
    r.atom()  # module_name
    for _ in range(r.leb()):
        r.atom()  # req
    for _ in range(r.leb()):  # exports
        t = r.u8()
        if t == 0:
            r.leb()
        else:
            r.leb(); r.atom()
        r.atom()  # export_name
    for _ in range(r.leb()):
        r.leb()  # stars
    for _ in range(r.leb()):
        r.leb(); r.atom(); r.leb()  # imports
    r.u8()  # has_tla
    functions = []
    assert r.u8() == TAG['FUNCTION_BYTECODE'], "module fn not bytecode"
    functions.append(r.fn(functions, atoms, 1))

    report = {'functions': []}
    for fn in functions:
        try:
            counts = decode(fn['code'], sizes, names)
        except ValueError as e:
            print(f"decode error: {e}", file=sys.stderr)
            return 1
        total = sum(counts.values())
        report['functions'].append({
            'code_bytes': len(fn['code']),
            'insns': total,
            'counts': counts,
        })

    if want_json:
        json.dump(report, sys.stdout, indent=1)
        return 0

    for fn in report['functions']:
        counts = fn['counts']
        total = fn['insns']
        print(f"function: {fn['code_bytes']} code bytes, {total} insns")
        print(f"{'op':<22} {'count':>5}  {'%':>6}")
        for op in sorted(counts, key=lambda o: -counts[o]):
            print(f"{names[op]:<22} {counts[op]:>5}  "
                  f"{100.0 * counts[op] / total:>5.2f}%")
        print()
    return 0


if __name__ == '__main__':
    sys.exit(main())
