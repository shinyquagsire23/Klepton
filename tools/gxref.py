#!/usr/bin/env python3
"""Cross-reference strings and code in a guest ELF .so.

Why this exists: the guest libraries are stripped of everything except .dynsym,
and the questions that come up in the M4/SL loops are nearly always of the form
"which function reads this string?" or "what strings does this function build?".
Answering either by hand in a 14 MB libmain.so is hours; answering it here is
seconds, and it is what turned "which argv option carries the Steam Link session
token?" into one command (the answer was `--cert`, in SDL_main, and the option
list itself came out of the second mode).

Two modes, both a linear pass over .text tracking `adrp`/`add`/`adr` pairs — the
only way an aarch64 PIC binary materialises the address of a constant:

  gxref.py <lib> --to <addr|string>   who references this address / string?
  gxref.py <lib> --in <sym|addr[,size]>   what strings does this code reference?

Use `--to=--server`, not `--to --server`: the strings worth chasing in these
binaries are mostly argv options, and argparse would eat the leading dashes.

Addresses are vaddrs. A bare --to argument that is not a number is looked up as
an exact NUL-terminated string in the file; note that a substring match finds
the WRONG address (the reference is to the start of the string, not into the
middle of it), so the search deliberately anchors to the preceding NUL.

Limitations worth knowing before trusting a zero result: a reference formed
across a branch, through a literal pool (`ldr xN, =addr`), or via a vtable/GOT
slot is invisible here. Zero hits means "not formed by adrp+add in .text", not
"never used" — check for the address as a data word before concluding anything.
"""
import argparse
import bisect
import sys

from elftools.elf.elffile import ELFFile


def load(path):
    f = open(path, 'rb')
    elf = ELFFile(f)
    text = next(s for s in elf.iter_sections() if s.name == '.text')
    syms = []
    for s in elf.iter_sections():
        if s.header['sh_type'] in ('SHT_SYMTAB', 'SHT_DYNSYM'):
            for sym in s.iter_symbols():
                if sym['st_value'] and sym['st_info']['type'] == 'STT_FUNC':
                    syms.append((sym['st_value'], sym['st_size'], sym.name))
    syms.sort()
    return elf, text, syms


def sym_at(syms, starts, addr):
    i = bisect.bisect_right(starts, addr) - 1
    if i >= 0:
        v, size, name = syms[i]
        if size == 0 or addr < v + size:
            return name, addr - v
    return '?', 0


def find_sym(syms, name):
    for v, size, n in syms:
        if n == name:
            return v, size
    for v, size, n in syms:               # substring, for mangled names
        if name in n:
            return v, size
    return None, None


def read_cstr(elf, addr, limit=160):
    for s in elf.iter_sections():
        lo = s['sh_addr']
        if not lo or s.header['sh_type'] != 'SHT_PROGBITS':
            continue
        if lo <= addr < lo + s['sh_size']:
            data = s.data()[addr - lo:addr - lo + limit]
            end = data.find(b'\0')
            return data[:end if end >= 0 else limit]
    return None


def string_addr(path, text):
    """Address of the NUL-terminated string whose content is `text`."""
    blob = open(path, 'rb').read()
    needle = text.encode() + b'\0'
    at = blob.find(needle)
    while at >= 0:
        # Anchor to the string's start: a hit in the middle of a longer string
        # is a different constant, and its address is not what the code loads.
        if at == 0 or blob[at - 1] == 0:
            return at
        at = blob.find(needle, at + 1)
    return None


def scan(text, lo, hi, on_pair):
    """Walk [lo, hi) tracking adrp/add/adr, calling on_pair(pc, value)."""
    data, base = text.data(), text['sh_addr']
    regs = {}
    for i in range((lo - base) // 4, (hi - base) // 4):
        word = int.from_bytes(data[i * 4:i * 4 + 4], 'little')
        pc = base + i * 4
        if (word & 0x9f000000) == 0x90000000:                      # adrp
            imm = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            regs[word & 31] = (pc & ~0xfff) + (imm << 12)
        elif (word & 0x9f000000) == 0x10000000:                    # adr
            imm = (((word >> 5) & 0x7ffff) << 2) | ((word >> 29) & 3)
            if imm & (1 << 20):
                imm -= 1 << 21
            regs[word & 31] = pc + imm
            on_pair(pc, regs[word & 31])
        elif (word & 0xff800000) == 0x91000000:                    # add imm, 64-bit
            rd, rn = word & 31, (word >> 5) & 31
            if rn in regs:
                regs[rd] = regs[rn] + (((word >> 10) & 0xfff) << (12 if (word >> 22) & 1 else 0))
                on_pair(pc, regs[rd])
            else:
                regs.pop(rd, None)
        else:
            regs.pop(word & 31, None)                              # clobbers Rd


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('lib')
    ap.add_argument('--to', help='address (0x...) or exact string to find references to')
    ap.add_argument('--in', dest='inside',
                    help='symbol name, or addr[,size], whose string references to list')
    args = ap.parse_args()
    if bool(args.to) == bool(args.inside):
        ap.error('give exactly one of --to / --in')

    elf, text, syms = load(args.lib)
    starts = [s[0] for s in syms]

    if args.to:
        try:
            target = int(args.to, 0)
        except ValueError:
            target = string_addr(args.lib, args.to)
            if target is None:
                sys.exit(f'no NUL-terminated string {args.to!r} in {args.lib}')
            print(f'{args.to!r} at {target:#x}')
        hits = []
        scan(text, text['sh_addr'], text['sh_addr'] + text['sh_size'],
             lambda pc, v: hits.append(pc) if v == target else None)
        for pc in hits:
            name, off = sym_at(syms, starts, pc)
            print(f'{pc:#x}  {name}+{off:#x}')
        print(f'{len(hits)} reference(s)')
        return

    if ',' in args.inside:
        addr, size = (int(x, 0) for x in args.inside.split(','))
    else:
        try:
            addr, size = int(args.inside, 0), None
        except ValueError:
            addr, size = find_sym(syms, args.inside)
            if addr is None:
                sys.exit(f'no such symbol: {args.inside}')
        if size is None:
            sys.exit('give a size as addr,size when the symbol has none')
    for pc, val in _strings_in(elf, text, addr, addr + size):
        print(f'{pc:#x}  {val!r}')


def _strings_in(elf, text, lo, hi):
    out = []

    def visit(pc, val):
        s = read_cstr(elf, val)
        if s and len(s) > 1 and all(32 <= c < 127 for c in s):
            out.append((pc, s.decode()))

    scan(text, lo, hi, visit)
    return out


if __name__ == '__main__':
    main()
