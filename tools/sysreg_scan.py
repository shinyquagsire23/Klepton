#!/usr/bin/env python3
"""Every system-register and cache-maintenance instruction in a guest ELF.

Trap 26 cost a device run because ONE instruction in ONE library — `mrs x9,
CTR_EL0` in libQt6Core's `__clear_cache` — is illegal from EL0 on Darwin, and
nothing below the device executes it. This is the preemptive version of that
question: what OTHER system registers do these guests touch, and which of them
can the kernel refuse?

It decodes the A64 system instruction group (bits[31:22] == 0b1101010100) into
its S<op0>_<op1>_C<n>_C<m>_<op2> form, names the ones we know, and sorts the
results by whether EL0 may execute them on Darwin:

  ok        architecturally EL0-accessible and measured working here
  TRAPS     EL0 access is disabled on Darwin — a SIGILL waiting for the run
            that takes that path (CTR_EL0 is the one that bit us)
  EL1       an EL1/EL2/EL3 register: UNDEFINED at EL0, always
  ?         not in the table — look it up before assuming it is fine

The classification is the point, not the disassembly: `objdump | grep mrs` gives
the same instructions and no verdict. Anything in TRAPS or ? needs a veneer
(runtime/kl_x18.c) or an explanation.

Usage: sysreg_scan.py <file.so> [more.so ...]
       sysreg_scan.py beatsaber/lib/arm64-v8a/*.so steamlink-vr/lib/arm64-v8a/*.so
"""
import struct, sys, array, os

# --- ELF64: the executable sections only ------------------------------------
# Segment-wide scanning finds "instructions" in .rodata and the relocation
# tables, which for libunity is 2 MB of false positives (trap 0's own lesson).
def exec_sections(data):
    if data[:4] != b'\x7fELF':
        return []
    e_shoff, = struct.unpack_from('<Q', data, 0x28)
    e_shentsize, e_shnum, e_shstrndx = struct.unpack_from('<HHH', data, 0x3a)
    if not e_shoff or not e_shnum:
        return []
    out = []
    for i in range(e_shnum):
        off = e_shoff + i * e_shentsize
        sh_type, = struct.unpack_from('<I', data, off + 4)
        sh_flags, sh_addr, sh_offset, sh_size = struct.unpack_from('<QQQQ', data, off + 8)
        if sh_type != 1 or not (sh_flags & 0x4):        # PROGBITS + EXECINSTR
            continue
        out.append((sh_addr, sh_offset, sh_size))
    return out

# --- the system group -------------------------------------------------------
# 1101 0101 00 L o0 op1 CRn CRm op2 Rt, with op0 = 2 + o0 for MRS/MSR and the
# 0b01 encoding being SYS/SYSL (DC, IC, AT, TLBI and friends).
def decode(w):
    if (w >> 22) != 0b1101010100:
        return None
    L    = (w >> 21) & 1
    op0f = (w >> 19) & 3
    op1  = (w >> 16) & 7
    crn  = (w >> 12) & 0xf
    crm  = (w >>  8) & 0xf
    op2  = (w >>  5) & 7
    rt   = w & 31
    if op0f & 2:                                        # MSR / MRS, register form
        return ('mrs' if L else 'msr', 2 + (op0f & 1), op1, crn, crm, op2, rt)
    if op0f == 1:                                       # SYS / SYSL
        return ('sysl' if L else 'sys', 1, op1, crn, crm, op2, rt)
    return None                                         # hints, barriers, MSR imm

# Name, and what EL0 may do with it on Darwin. Everything measured on this
# machine unless marked; see notes/TRAPS.md trap 26 for how CTR_EL0 was found.
SYSREG = {
    # (op0, op1, crn, crm, op2): (name, verdict)
    (3, 3, 0, 0, 1): ('CTR_EL0',    'handled'),  # trap 26 — veneered (kl_x18.c)
    (3, 3, 0, 0, 7): ('DCZID_EL0',  'ok'),
    (3, 3, 13, 0, 2): ('TPIDR_EL0', 'handled'),  # trap 1 — rewritten to TPIDRRO
    (3, 3, 13, 0, 3): ('TPIDRRO_EL0', 'ok'),
    (3, 3, 14, 0, 0): ('CNTFRQ_EL0', 'ok'),
    (3, 3, 14, 0, 1): ('CNTPCT_EL0', 'ok'),
    (3, 3, 14, 0, 2): ('CNTVCT_EL0', 'ok'),
    (3, 3, 4, 4, 0): ('FPCR',       'ok'),
    (3, 3, 4, 4, 1): ('FPSR',       'ok'),
    (3, 3, 4, 2, 0): ('NZCV',       'ok'),
    (3, 3, 4, 2, 1): ('DAIF',       'TRAPS'),   # EL0 may not read DAIF
    (3, 3, 9, 12, 0): ('PMCR_EL0',  'TRAPS'),
    (3, 3, 9, 13, 0): ('PMCCNTR_EL0', 'TRAPS'),
    (3, 0, 0, 0, 0): ('MIDR_EL1',   'EL1'),
    (3, 0, 0, 0, 5): ('MPIDR_EL1',  'EL1'),
    (3, 0, 0, 6, 0): ('ID_AA64ISAR0_EL1', 'EL1'),
    (3, 0, 0, 6, 1): ('ID_AA64ISAR1_EL1', 'EL1'),
    (3, 0, 0, 4, 0): ('ID_AA64PFR0_EL1',  'EL1'),
    (3, 0, 0, 4, 1): ('ID_AA64PFR1_EL1',  'EL1'),
    (3, 0, 0, 7, 0): ('ID_AA64MMFR0_EL1', 'EL1'),
    (3, 0, 1, 0, 0): ('SCTLR_EL1',  'EL1'),
}
# SYS aliases worth naming; DC/IC operate at EL0 through SCTLR_EL1.UCI, which
# Darwin leaves ENABLED — measured, and it is why sys_icache_invalidate works.
SYSOP = {
    (1, 3, 7, 11, 1): ('dc cvau',  'ok'),
    (1, 3, 7, 5, 1):  ('ic ivau',  'ok'),
    (1, 3, 7, 5, 0):  ('ic iallu', 'EL1'),
    (1, 3, 7, 10, 1): ('dc cvac',  'ok'),
    (1, 3, 7, 14, 1): ('dc civac', 'ok'),
    (1, 3, 7, 4, 1):  ('dc zva',   'ok'),
    (1, 3, 7, 6, 1):  ('dc ivac',  'EL1'),
}

def name_of(kind, op0, op1, crn, crm, op2):
    if kind in ('sys', 'sysl'):
        n = SYSOP.get((1, op1, crn, crm, op2))
        if n:
            return n
        return (f'sys #{op1}, c{crn}, c{crm}, #{op2}', '?')
    n = SYSREG.get((op0, op1, crn, crm, op2))
    if n:
        return n
    return (f'S{op0}_{op1}_C{crn}_C{crm}_{op2}', '?')

# Trap 0b/0d, the same window test kl_x18.c uses: an executable SECTION can
# contain data, and a constant table hits these encodings by chance. Without
# this every crypto table in the tree reports a handful of exotic `sys #6, c2,
# c3, #3`-shaped entries that are not instructions at all — noise in exactly the
# column a reader is meant to act on.
WIN, LIMIT = 16, 2
def looks_like_data(words, i):
    lo, hi = max(0, i - WIN), min(len(words), i + WIN + 1)
    bad = 0
    for k in range(lo, hi):
        if ((words[k] >> 25) & 0xf) <= 3:            # unallocated / SVE
            bad += 1
            if bad >= LIMIT:
                return True
    return False

def scan(path):
    data = open(path, 'rb').read()
    secs = exec_sections(data)
    if not secs:
        print(f'{path}: no executable sections (stripped?) — skipped', file=sys.stderr)
        return {}
    hits = {}
    for sh_addr, sh_offset, sh_size in secs:
        n = sh_size // 4
        words = array.array('I')
        words.frombytes(data[sh_offset:sh_offset + n * 4])
        for i, w in enumerate(words):
            d = decode(w)
            if not d:
                continue
            kind, op0, op1, crn, crm, op2, rt = d
            nm, verdict = name_of(kind, op0, op1, crn, crm, op2)
            if looks_like_data(words, i):
                verdict = 'data'
            key = (kind, nm, verdict)
            hits.setdefault(key, []).append(sh_addr + i * 4)
    return hits

def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    total = {}
    worst = 0
    for p in argv[1:]:
        hits = scan(p)
        if not hits:
            continue
        rank = {'?': 0, 'TRAPS': 1, 'EL1': 2, 'handled': 3, 'ok': 4, 'data': 5}
        rows = sorted(hits.items(), key=lambda kv: (rank.get(kv[0][2], 0), kv[0][1]))
        interesting = [r for r in rows if r[0][2] in ('?', 'TRAPS', 'EL1')]
        print(f'=== {p} ===')
        for (kind, nm, verdict), addrs in rows:
            mark = '' if verdict == 'ok' else '   <-- ' + verdict
            where = ', '.join(hex(a) for a in addrs[:4])
            if len(addrs) > 4:
                where += f', +{len(addrs) - 4} more'
            print(f'  {kind:5s} {nm:<22s} x{len(addrs):<5d} {where}{mark}')
            k = (kind, nm, verdict)
            total[k] = total.get(k, 0) + len(addrs)
        if interesting:
            worst += 1
        print()
    print('=== everything, across all files ===')
    rank = {'?': 0, 'TRAPS': 1, 'EL1': 2, 'handled': 3, 'ok': 4, 'data': 5}
    for (kind, nm, verdict), n in sorted(total.items(),
                                         key=lambda kv: (rank.get(kv[0][2], 0), kv[0][1])):
        mark = '' if verdict == 'ok' else '   <-- ' + verdict
        print(f'  {kind:5s} {nm:<22s} x{n}{mark}')
    # 'handled' is TPIDR_EL0 (trap 1, rewritten at load) and CTR_EL0 (trap 26,
    # veneered) — real traps with a fix already in the tree, so counting them
    # here would bury the ones that have neither.
    bad = sum(n for (k, nm, v), n in total.items() if v in ('?', 'TRAPS', 'EL1'))
    done = sum(n for (k, nm, v), n in total.items() if v == 'handled')
    print(f'\n{bad} instruction(s) EL0 may not execute and NOTHING handles, '
          f'in {worst} file(s); {done} more are handled (traps 1 and 26).'
          if bad else f'\nnothing unhandled. {done} instruction(s) EL0 may not '
                      f'execute, all covered by traps 1 and 26.')
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
