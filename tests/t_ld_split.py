#!/usr/bin/env python3
"""klepton-ld SPLIT placement, on a synthetic library wider than two `b` windows.

Deadpool VR's libUE4.so (128 MB .text, 262 MB image) was the first guest no
single veneer pool could serve. SPLIT placement gives the translator a pool
below the image AND one above it, chosen per 32 MB slice, halving a slice
neither reaches, and keeps a halved slice instruction-aligned when it comes
from an odd-word tail. No library on a typical host tree is that wide, so this
builds one: a sparse ELF64
AArch64 .so whose two executable sections span ~300 MB, with `mov x0, x18`
sites only where some pool can reach them, and a first section whose length is
an odd number of words ending inside the band neither pool reaches — so the
tail slice MUST be halved from an odd size.

Pass = klepton-ld succeeds, says SPLIT, veneers every site, refuses none, and
reports at least one odd-word halving re-aligned. Without SPLIT placement the
translator dies with "no single veneer pool"; without the alignment step there
is no odd-word accounting line.

usage: t_ld_split.py <klepton-ld> <scratch-dir>
"""
import os, re, struct, subprocess, sys

MB = 1 << 20
MOV_X0_X18 = 0xAA1203E0          # orr x0, xzr, x18
RET = 0xD65F03C0

def build(path):
    page = 0x4000
    a_lo = 0x4000
    a_len = 140 * MB + 4          # odd number of words: the tail slice is odd
    b_lo = 160 * MB               # second exec section, word-aligned
    b_len = 140 * MB
    text_end = b_lo + b_len
    rw_lo = (text_end + page - 1) & ~(page - 1)
    rw_len = page
    sites = [a_lo, a_lo + 60 * MB, b_lo + 60 * MB, b_lo + b_len - 8]
    shstr = b"\0.text\0.text.b\0.data\0.shstrtab\0"
    shstr_off = rw_lo + rw_len
    sh_off = (shstr_off + len(shstr) + 7) & ~7
    with open(path, "wb") as f:
        f.truncate(sh_off + 5 * 64)
        # ELF header
        ident = b"\x7fELF" + bytes([2, 1, 1, 0]) + bytes(8)
        f.seek(0)
        f.write(ident + struct.pack("<HHIQQQIHHHHHH", 3, 183, 1, 0, 64, sh_off, 0,
                                    64, 56, 2, 64, 5, 4))
        # PT_LOAD r-x [0, text_end), PT_LOAD rw [rw_lo, +rw_len)
        f.write(struct.pack("<IIQQQQQQ", 1, 5, 0, 0, 0, text_end, text_end, page))
        f.write(struct.pack("<IIQQQQQQ", 1, 6, rw_lo, rw_lo, rw_lo, rw_len, rw_len, page))
        for s in sites:
            f.seek(s); f.write(struct.pack("<II", MOV_X0_X18, RET))
        f.seek(shstr_off); f.write(shstr)
        f.seek(sh_off)
        def sh(name, typ, flags, addr, size, align):
            f.write(struct.pack("<IIQQQQIIQQ", name, typ, flags, addr, addr, size, 0, 0, align, 0))
        f.write(bytes(64))
        sh(1, 1, 6, a_lo, a_len, 4)            # .text     AX
        sh(7, 1, 6, b_lo, b_len, 4)            # .text.b   AX
        sh(15, 1, 3, rw_lo, rw_len, 8)         # .data     WA
        f.write(struct.pack("<IIQQQQIIQQ", 21, 3, 0, 0, shstr_off, len(shstr), 0, 0, 1, 0))
    return len(sites)

def main():
    ld, scratch = sys.argv[1], sys.argv[2]
    os.makedirs(scratch, exist_ok=True)
    so, out = os.path.join(scratch, "wide.so"), os.path.join(scratch, "wide.dylib")
    fails = 0
    def ck(ok, what):
        nonlocal fails
        print(("  ok   " if ok else "  FAIL ") + what)
        fails += 0 if ok else 1
    try:
        n = build(so)
        r = subprocess.run([ld, so, "-o", out], capture_output=True, text=True)
        log = r.stdout + r.stderr
        ck(r.returncode == 0, "klepton-ld translates a library wider than two b windows")
        if r.returncode != 0:
            print(log)
        ck("SPLIT placement" in log, "it chose SPLIT placement")
        m = re.search(r"x18 sites\s+(\d+)\s+veneered (\d+)\s+refused (\d+)", log)
        ck(bool(m) and int(m.group(1)) == n and int(m.group(2)) == n and int(m.group(3)) == 0,
           f"all {n} x18 sites veneered, none refused ({m.group(0) if m else 'no stats line'})")
        h = re.search(r"slice halved (\d+) times \((\d+) from an odd-word tail", log)
        ck(bool(h) and int(h.group(2)) > 0,
           f"an odd-word tail slice was halved and re-aligned ({h.group(0) if h else 'no halving line'})")
    finally:
        for p in (so, out):
            try: os.unlink(p)
            except OSError: pass
    print("FAILED (%d)" % fails if fails else "all good")
    return 1 if fails else 0

if __name__ == "__main__":
    sys.exit(main())
