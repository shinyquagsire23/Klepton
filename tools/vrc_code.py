#!/usr/bin/env python3
"""Read VRChat's CODE: managed method <-> address, and what a method touches.

Why this exists
---------------
`tools/vrc_metadata.py` made the managed side readable — 32,168 types, 302,349
methods, every token and RID. It could not say where any of them IS, and this
target has no symbols to ask: `libil2cpp.so` is 297 MB with 2241 dynamic symbols
(3 of them `il2cpp_*`, the rest renamed), VRChat's own method names are
obfuscated to runs of `ÌÍÎÏ`, and the .text makes no direct PLT calls. So "which
code shows the Under Construction screen?" had no starting point at all.

Three facts close that, and each is a table already in the binary:

  * `Il2CppCodeGenModule.methodPointers[rid - 1]` is the address of the method
    whose token is `0x06000000 | rid`. One module per image, found by the
    pointer to its own `"<name>.dll"` string.
  * Every pointer field in this image is stored as ZERO in the file and supplied
    by an `R_AARCH64_RELATIVE` relocation's addend, so searching the file bytes
    for a pointer finds nothing (it was tried). The relocation table IS the
    pointer table, and it also answers "who points at this?" backwards.
  * A metadata usage slot holds `(kind << 29) | (index << 1) | 1` in the FILE
    until the owning method's one-time init overwrites it with the real pointer.
    kind 5 is a string literal, 3 a method, 1/2 a type. So a slot decodes to a
    NAME, and the `adrp`+`ldr`/`add` that reaches it decodes back to the method
    that uses it — which is how a screen path string leads to the coroutine that
    shows it.

Nothing here needs the process to be running, and nothing here writes anything.

Usage
-----
    vrc_code.py --whois 0x698d7d0            an address -> Type::method +off
    vrc_code.py --type '^VRCUiManager$'      a type's methods, with addresses
    vrc_code.py --usages 0x698d634           what a method touches, decoded
    vrc_code.py --literals 'Screens/Auth'    the managed "..." constants
    vrc_code.py --uses-literal 16744         who READS a literal
    vrc_code.py --uses-type 'VRCUiUnderCon'  who references a type
    vrc_code.py --callers 0x6fdd650          callers, with a literal argument
    vrc_code.py --modules                    every image, with its method count

Addresses are guest vaddrs in `libil2cpp.so` — the same numbers `objdump
--start-address` wants, and the same ones runtime/guest/kl_guestpatch.c records.
"""
import argparse
import importlib.util
import os
import re
import struct
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
SO = 'vrchat/lib/arm64-v8a/libil2cpp.so'

# `Il2CppMetadataRegistration`, found by the only property that identifies it
# without symbols: two of its (count, pointer) pairs point at pointer ARRAYS
# whose length equals the count sitting 8 bytes before them. From that anchor
# every other field is at its documented offset, and `typeDefinitionsSizesCount`
# reading 32168 — the metadata's own type count, derived independently — is the
# check that the base is right.
METADATA_REGISTRATION = 0x1104f310
# ...and the head of the codeGenModules array, the same way: 228 consecutive
# pointers to structs whose first field points at a "*.dll" string.
CODEGEN_MODULES = 0x113c46c0

KIND = {1: 'TypeInfo', 2: 'Il2CppType', 3: 'MethodDef', 4: 'FieldInfo',
        5: 'StringLiteral', 6: 'MethodRef'}


def _load_metadata():
    spec = importlib.util.spec_from_file_location(
        'vrc_metadata', os.path.join(HERE, 'vrc_metadata.py'))
    vm = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(vm)
    return vm, vm.Metadata(vm.DEFAULT_SRC)


class Image:
    """libil2cpp.so, with its relocations resolved and its .text as words."""

    def __init__(self, path=SO):
        self.raw = open(path, 'rb').read()
        raw = self.raw
        ph, = struct.unpack_from('<Q', raw, 0x20)
        ent, num = struct.unpack_from('<HH', raw, 0x36)
        self.segs, self.text = [], None
        for i in range(num):
            o = ph + i * ent
            t, = struct.unpack_from('<I', raw, o)
            off, va, _pa, fsz, _msz, _al = struct.unpack_from('<6Q', raw, o + 8)
            if t == 1:
                self.segs.append((va, off, fsz))
                if not self.text:                     # the executable LOAD
                    pass
            if t == 2:
                self.dyn = (off, fsz)
        # DT_RELA / DT_RELASZ, rather than assuming the section layout
        off, fsz = self.dyn
        rela = relasz = None
        for i in range(fsz // 16):
            tag, val = struct.unpack_from('<Qq', raw, off + i * 16)
            if tag == 7:
                rela = val
            elif tag == 8:
                relasz = val
            elif tag == 0:
                break
        rel = np.frombuffer(raw, dtype=np.uint64, count=relasz // 8, offset=rela)
        m = (rel[1::3] & 0xffffffff) == 1027          # R_AARCH64_RELATIVE
        r_off, r_add = rel[0::3][m], rel[2::3][m]
        order = np.argsort(r_off)
        self.r_off, self.r_add = r_off[order], r_add[order]

        # the executable segment, as instruction words
        exe = max(self.segs, key=lambda s: s[2])
        self.text_va, text_off, text_sz = exe
        align = (-self.text_va) % 4
        self.W = np.frombuffer(raw, dtype=np.uint32, offset=text_off + align,
                               count=(text_sz - align) // 4)
        self.PC0 = self.text_va + align

    # --- addresses ---------------------------------------------------------
    def v2o(self, va):
        for v, o, f in self.segs:
            if v <= va < v + f:
                return o + (va - v)
        return None

    def ptr_at(self, va):
        k = np.searchsorted(self.r_off, va)
        return int(self.r_add[k]) if k < len(self.r_off) and self.r_off[k] == va else None

    def refs_to(self, va):
        return [int(x) for x in self.r_off[self.r_add == va]]

    def u32(self, va):
        return struct.unpack_from('<I', self.raw, self.v2o(va))[0]

    def u64(self, va):
        return struct.unpack_from('<Q', self.raw, self.v2o(va))[0]

    def cstr(self, va):
        o = self.v2o(va)
        return self.raw[o:self.raw.index(b'\0', o)].decode('utf-8', 'replace')


class Code:
    """The join: metadata names <-> image addresses."""

    def __init__(self):
        self.img = Image()
        self.vm, self.md = _load_metadata()
        img, md = self.img, self.md

        # every Il2CppCodeGenModule: (name, methodPointerCount, methodPointers)
        self.modules = []
        k = 0
        while True:
            p = img.ptr_at(CODEGEN_MODULES + 8 * k)
            if p is None:
                break
            nm = img.ptr_at(p)
            s = img.cstr(nm) if nm else None
            if not (s and s.endswith('.dll')):
                break
            self.modules.append((s, img.u32(p + 8), img.ptr_at(p + 0x10)))
            k += 1

        # (image, rid) -> (type name, method name)
        self.by_rid = {}
        for i in range(md.t['images'][1] // 40):
            im = md.image(i)
            tbl = self.by_rid.setdefault(md.string(im['nameIndex']), {})
            for t in range(im['typeStart'], im['typeStart'] + im['typeCount']):
                td = md.type_def(t)
                tn = md.type_name(t)
                for j in range(td['method_count']):
                    m = md.method(td['methodStart'] + j)
                    tbl[m['token'] & 0xffffff] = (tn, md.string(m['nameIndex']))

        # address -> (image, rid), sorted, so an arbitrary pc resolves
        addrs, tags = [], []
        for name, count, mp in self.modules:
            if not mp or not count:
                continue
            lo = np.searchsorted(img.r_off, mp)
            hi = np.searchsorted(img.r_off, mp + 8 * count)
            offs, adds = img.r_off[lo:hi], img.r_add[lo:hi]
            addrs.append(adds.astype(np.int64))
            tags.extend((name, int(r)) for r in ((offs - mp) // 8 + 1))
        self.addr = np.concatenate(addrs)
        order = np.argsort(self.addr, kind='stable')
        self.addr = self.addr[order]
        self.tag = [tags[i] for i in order]

        # the Il2CppType table, for decoding TypeInfo usages and declaring types
        self.types = img.ptr_at(METADATA_REGISTRATION + 0x38)
        self.ntypes = img.u32(METADATA_REGISTRATION + 0x30)
        lo = np.searchsorted(img.r_off, self.types)
        hi = np.searchsorted(img.r_off, self.types + 8 * self.ntypes)
        self.type_ptr = np.zeros(self.ntypes, dtype=np.int64)
        self.type_ptr[((img.r_off[lo:hi] - self.types) // 8).astype(np.int64)] = \
            img.r_add[lo:hi]

    # --- names -------------------------------------------------------------
    def method_addr(self, rid, module='Assembly-CSharp.dll'):
        for name, count, mp in self.modules:
            if name == module:
                return self.img.ptr_at(mp + 8 * (rid - 1)) if 1 <= rid <= count else None
        return None

    def whois(self, va):
        k = int(np.searchsorted(self.addr, va, side='right')) - 1
        if k < 0:
            return None
        start = int(self.addr[k])
        mod, rid = self.tag[k]
        tn, mn = self.by_rid.get(mod, {}).get(rid, ('?', '?'))
        end = int(self.addr[k + 1]) if k + 1 < len(self.addr) else start
        return dict(start=start, end=end, delta=va - start, module=mod, rid=rid,
                    type=tn, name=mn)

    def fmt(self, va):
        w = self.whois(va)
        if not w:
            return '0x%09x <no method>' % va
        return ('0x%09x = %s::%s +0x%x  [%s rid %d]'
                % (va, w['type'], w['name'], w['delta'], w['module'], w['rid']))

    def il2type(self, i):
        p = int(self.type_ptr[i]) if 0 <= i < self.ntypes else 0
        if not p:
            return None
        o = self.img.v2o(p)
        data = struct.unpack_from('<Q', self.img.raw, o)[0]
        bits = struct.unpack_from('<I', self.img.raw, o + 8)[0]
        return dict(data=data, enum=(bits >> 16) & 0xff)

    def type_of(self, i):
        """An Il2CppType index as a readable name, where it names a class."""
        t = self.il2type(i)
        if t and t['enum'] in (0x11, 0x12) and t['data'] < self.md.type_count():
            return self.md.type_name(t['data'])
        return 'Il2CppType[%d]%s' % (i, '' if not t else ' (enum 0x%02x)' % t['enum'])

    def literal(self, i):
        off = self.md.t['stringLiteral'][0] + i * 8
        ln, di = struct.unpack_from('<ii', self.md.data, off)
        dat = self.md.t['stringLiteralData'][0]
        return self.md.data[dat + di:dat + di + ln].decode('utf-8', 'replace')

    # --- slots -------------------------------------------------------------
    def decode_slot(self, va):
        o = self.img.v2o(va)
        if o is None:
            return None
        v = struct.unpack_from('<Q', self.img.raw, o)[0]
        if not (v & 1) or v > 0xffffffff:
            return None                       # already a pointer, or not a slot
        kind, idx = (v >> 29) & 7, (v >> 1) & 0x0fffffff
        if kind == 5:
            return 'StringLiteral[%d] %r' % (idx, self.literal(idx))
        if kind == 3 and idx < self.md.method_count():
            m = self.md.method(idx)
            return 'MethodDef[%d] %s::%s' % (idx, self.type_of(m['declaringType']),
                                             self.md.string(m['nameIndex']))
        if kind in (1, 2):
            return '%s %s' % (KIND[kind], self.type_of(idx))
        return '%s[%d]' % (KIND.get(kind, kind), idx)

    def slots_encoding(self, val):
        pat = struct.pack('<Q', val)
        out, s = [], 0
        while True:
            p = self.img.raw.find(pat, s)
            if p < 0:
                return out
            s = p + 8
            for v, off, f in self.img.segs:
                if off <= p < off + f:
                    out.append(v + (p - off))

    # --- instructions ------------------------------------------------------
    def _adrp(self):
        if not hasattr(self, '_adrp_cache'):
            W = self.img.W
            sel = ((W >> 31) & 1).astype(bool) & (((W >> 24) & 0x1f) == 0x10)
            idx = np.nonzero(sel)[0]
            w = W[idx].astype(np.int64)
            imm = (((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)
            imm = np.where(imm >= (1 << 20), imm - (1 << 21), imm)
            pc = self.img.PC0 + idx * 4
            self._adrp_cache = (pc, (w & 0x1f), (pc & ~0xfff) + (imm << 12))
        return self._adrp_cache

    def _pair(self, i, rd, window=6):
        """(offset, use pc) for the ldr/add that completes an adrp at word i."""
        W = self.img.W
        for k in range(1, window + 1):
            if i + k >= len(W):
                return None
            w = int(W[i + k])
            if (w >> 22) & 0x3ff in (0x3e5, 0x3e4) and ((w >> 5) & 0x1f) == rd:
                return ((w >> 10) & 0xfff) * 8, self.img.PC0 + (i + k) * 4
            if (w >> 23) & 0x1ff == 0x122 and ((w >> 5) & 0x1f) == rd:
                return (w >> 10) & 0xfff, self.img.PC0 + (i + k) * 4
        return None

    def readers_of(self, va):
        """Instruction addresses that adrp+ldr/add exactly `va`."""
        PC, RD, TGT = self._adrp()
        page, off = va & ~0xfff, va & 0xfff
        out = []
        for c in np.nonzero(TGT == page)[0]:
            pc, rd = int(PC[c]), int(RD[c])
            got = self._pair((pc - self.img.PC0) // 4, rd)
            if got and got[0] == off:
                out.append(got[1])
        return out

    def usages_in(self, lo, hi):
        """(pc, slot, decoded) for every metadata slot a range reaches."""
        W = self.img.W
        i0, i1 = (lo - self.img.PC0) // 4, (hi - self.img.PC0) // 4
        w = W[i0:i1].astype(np.int64)
        sel = ((w >> 31) & 1).astype(bool) & (((w >> 24) & 0x1f) == 0x10)
        out = []
        for k in np.nonzero(sel)[0]:
            wi = int(w[k])
            imm = (((wi >> 5) & 0x7ffff) << 2) | ((wi >> 29) & 3)
            if imm >= (1 << 20):
                imm -= 1 << 21
            pc = lo + int(k) * 4
            got = self._pair(i0 + int(k), wi & 0x1f)
            if not got:
                continue
            slot = (pc & ~0xfff) + (imm << 12) + got[0]
            d = self.decode_slot(slot)
            if d:
                out.append((got[1], slot, d))
        return out

    def callers_of(self, va):
        W = self.img.W
        out = []
        for op, kind in ((0x25, 'bl'), (0x05, 'b')):
            idx = np.nonzero(((W >> 26) & 0x3f) == op)[0]
            imm = (W[idx] & 0x3ffffff).astype(np.int64)
            imm = np.where(imm >= (1 << 25), imm - (1 << 26), imm)
            pc = self.img.PC0 + idx * 4
            out += [(int(p), kind) for p in pc[(pc + imm * 4) == va]]
        return sorted(out)


def main():
    ap = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--whois', metavar='ADDR')
    ap.add_argument('--type', metavar='REGEX')
    ap.add_argument('--usages', metavar='ADDR')
    ap.add_argument('--literals', metavar='REGEX')
    ap.add_argument('--uses-literal', metavar='INDEX', type=lambda s: int(s, 0))
    ap.add_argument('--uses-type', metavar='REGEX')
    ap.add_argument('--callers', metavar='ADDR')
    ap.add_argument('--modules', action='store_true')
    a = ap.parse_args()
    c = Code()

    if a.modules:
        for name, count, mp in sorted(c.modules, key=lambda m: -m[1]):
            print('  %-45s %6d methods  methodPointers 0x%09x'
                  % (name, count, mp or 0))
        print('%d image(s)' % len(c.modules))

    if a.whois:
        print(c.fmt(int(a.whois, 0)))

    if a.type:
        rx = re.compile(a.type)
        for i in range(c.md.type_count()):
            name = c.md.type_name(i)
            if not rx.search(name):
                continue
            td = c.md.type_def(i)
            print('%s  (type %d, token 0x%08x, %d methods)'
                  % (name, i, td['token'], td['method_count']))
            for k in range(td['method_count']):
                m = c.md.method(td['methodStart'] + k)
                rid = m['token'] & 0xffffff
                print('    rid %-6d %-46s -> 0x%09x  params %d'
                      % (rid, c.md.string(m['nameIndex'])[:46],
                         c.method_addr(rid) or 0, m['parameterCount']))

    if a.usages:
        va = int(a.usages, 0)
        w = c.whois(va)
        lo, hi = w['start'], w['end']
        print('%s   [0x%x .. 0x%x, %d bytes]' % (c.fmt(lo), lo, hi, hi - lo))
        seen = set()
        for pc, slot, d in c.usages_in(lo, hi):
            if (slot, d) in seen:
                continue
            seen.add((slot, d))
            print('   @0x%09x  slot 0x%09x  %s' % (pc, slot, d))

    if a.literals:
        rx = re.compile(a.literals, re.I)
        n = c.md.t['stringLiteral'][1] // 8
        hits = 0
        for i in range(n):
            s = c.literal(i)
            if rx.search(s):
                print('[%6d] %s' % (i, s))
                hits += 1
        print('%d literal(s)' % hits)

    if a.uses_literal is not None:
        for slot in c.slots_encoding((5 << 29) | (a.uses_literal << 1) | 1):
            print('StringLiteral[%d] %r -> slot 0x%09x'
                  % (a.uses_literal, c.literal(a.uses_literal), slot))
            for pc in c.readers_of(slot):
                print('   %s' % c.fmt(pc))

    if a.uses_type:
        rx = re.compile(a.uses_type)
        want = [i for i in range(c.md.type_count()) if rx.search(c.md.type_name(i))]
        for td_index in want:
            print('=== type %d %s' % (td_index, c.md.type_name(td_index)))
            for ti in range(c.ntypes):
                t = c.il2type(ti)
                if not (t and t['enum'] in (0x11, 0x12) and t['data'] == td_index):
                    continue
                for kind in (1, 2):
                    for slot in c.slots_encoding((kind << 29) | (ti << 1) | 1):
                        print('  %s Il2CppType[%d] -> slot 0x%09x'
                              % (KIND[kind], ti, slot))
                        for pc in c.readers_of(slot):
                            print('     %s' % c.fmt(pc))

    if a.callers:
        va = int(a.callers, 0)
        print('callers of %s' % c.fmt(va))
        for pc, kind in c.callers_of(va):
            w = c.whois(pc)
            lits = [d for _p, _s, d in c.usages_in(max(pc - 0x60, w['start']), pc)
                    if d.startswith('StringLiteral')] if w else []
            print('  0x%09x %-3s %-62s %s'
                  % (pc, kind,
                     '%s::%s +0x%x' % (w['type'], w['name'], w['delta']) if w else '?',
                     lits[-1] if lits else ''))
    return 0


if __name__ == '__main__':
    sys.exit(main())
