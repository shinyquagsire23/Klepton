#!/usr/bin/env python3
"""What a tree of Linux/glibc ARM64 ELFs asks of a loader, before any of it runs.

The Android guests answer these questions through `make check`; a glibc guest
has no APK to hang that on, and the answers decide the design: is the launcher
a PIE, which TLS models and relocation types does it use, how many thread-pointer
reads and raw syscalls need veneers, which glibc does it want, and which VR,
windowing and scripting APIs does it name.

Usage: audit_elf.py [--runtime <sysroot-lib-dir>]... [-o report.md] <dir-or-file>...

`--runtime` directories are only searched for the providers of imported symbols
and of DT_NEEDED names; they are not audited themselves.
"""
import argparse, array, os, re, struct, sys
from collections import Counter, defaultdict

EM_AARCH64 = 183
PT_LOAD, PT_DYNAMIC, PT_INTERP, PT_TLS = 1, 2, 3, 7
SHT_RELA, SHT_DYNSYM, SHT_RELR = 4, 11, 19
SHT_VERSYM, SHT_VERNEED = 0x6fffffff, 0x6ffffffe
DT_NEEDED, DT_SONAME, DT_FLAGS, DT_FLAGS_1 = 1, 14, 30, 0x6ffffffb
DF_STATIC_TLS, DF_1_PIE = 0x10, 0x08000000

RELOC = {257: 'ABS64', 1024: 'COPY', 1025: 'GLOB_DAT', 1026: 'JUMP_SLOT',
         1027: 'RELATIVE', 1028: 'TLS_DTPMOD', 1029: 'TLS_DTPREL',
         1030: 'TLS_TPREL', 1031: 'TLSDESC', 1032: 'IRELATIVE'}
# What runtime/kl_image.c applies today; everything else is a load failure there.
RELOC_HANDLED = {'ABS64', 'GLOB_DAT', 'JUMP_SLOT', 'RELATIVE'}

# Names that decide a strategy, searched in the raw bytes of every file.
PROBES = [
    ('OpenVR',    rb'IVR[A-Za-z]+_\d{3}|VR_Init(?:Internal\d?)?|openvr_api|vrclient'),
    ('OpenXR',    rb'xrCreateInstance|XR_(?:KHR|EXT|FB|META|VALVE|MND|HTC)_[a-z0-9_]+'),
    ('Vulkan',    rb'VK_(?:KHR|EXT|QCOM|ARM|IMG|MVK|NV|AMD|VALVE)_[a-z0-9_]+'),
    ('SDL',       rb'SDL_(?:Init|CreateWindow|Vulkan_[A-Za-z]+|GetRevision|VIDEODRIVER|AUDIODRIVER|VIDEO_DRIVER|AUDIO_DRIVER)|libSDL[23][^\x00]{0,12}\.so[.0-9]*'),
    ('JIT',       rb'v8::|V8_Fatal|--?jitless|libv8[^\x00]{0,16}|LuaJIT [0-9.]+|luaJIT_|mono_jit|PROT_EXEC'),
    ('Steam',     rb'SteamAPI_[A-Za-z_]+|libsteam_api\.so|steamclient\.so|SteamClient\d{3}'),
    ('Audio',     rb'libpulse[^\x00]{0,16}|libpipewire[^\x00]{0,16}|libasound[^\x00]{0,12}|snd_pcm_open|pa_stream_new|pw_stream_new'),
    ('Windowing', rb'libX11[^\x00]{0,10}|libwayland-client[^\x00]{0,10}|libxcb[^\x00]{0,10}|XOpenDisplay|wl_display_connect'),
]


def cstr(data, off):
    end = data.find(b'\0', off)
    return data[off:end].decode('latin-1')


class Elf:
    def __init__(self, path):
        self.path = path
        with open(path, 'rb') as f:
            self.data = d = f.read()
        self.ok = (len(d) > 0x40 and d[:4] == b'\x7fELF' and d[4] == 2
                   and struct.unpack_from('<H', d, 0x12)[0] == EM_AARCH64)
        if not self.ok:
            return
        self.e_type = struct.unpack_from('<H', d, 0x10)[0]
        self.e_entry, phoff, shoff = struct.unpack_from('<QQQ', d, 0x18)
        phentsize, phnum, shentsize, shnum, _ = struct.unpack_from('<HHHHH', d, 0x36)
        self.ph = [struct.unpack_from('<IIQQQQQQ', d, phoff + i * phentsize) for i in range(phnum)]
        # (name, type, flags, addr, offset, size, link, info, addralign, entsize)
        self.sh = [struct.unpack_from('<IIQQQQIIQQ', d, shoff + i * shentsize)
                   for i in range(shnum)] if shoff else []
        loads = [p for p in self.ph if p[0] == PT_LOAD]
        self.first_vaddr = loads[0][3] if loads else None
        self.p_align = max((p[7] for p in loads), default=0)
        self.interp = next((cstr(d, p[2]) for p in self.ph if p[0] == PT_INTERP), None)
        tls = next((p for p in self.ph if p[0] == PT_TLS), None)
        self.tls = (tls[6], tls[7]) if tls else None          # memsz, align
        self._dynamic()
        self._symbols()
        self._relocs()
        self._code()

    def _off(self, vaddr):
        for p in self.ph:
            if p[0] == PT_LOAD and p[3] <= vaddr < p[3] + p[5]:
                return vaddr - p[3] + p[2]
        return None

    def _dynamic(self):
        self.needed, self.soname, self.flags, self.flags1 = [], None, 0, 0
        dyn = next((p for p in self.ph if p[0] == PT_DYNAMIC), None)
        if not dyn:
            return
        ents = [struct.unpack_from('<qQ', self.data, dyn[2] + i * 16) for i in range(dyn[5] // 16)]
        strtab = self._off(next((v for t, v in ents if t == 5), 0) or 0)
        for t, v in ents:
            if t == 0:
                break
            if t == DT_NEEDED and strtab is not None:
                self.needed.append(cstr(self.data, strtab + v))
            elif t == DT_SONAME and strtab is not None:
                self.soname = cstr(self.data, strtab + v)
            elif t == DT_FLAGS:
                self.flags = v
            elif t == DT_FLAGS_1:
                self.flags1 = v

    def _symbols(self):
        """Imports as (name, version) and exports as names, from .dynsym."""
        self.imports, self.exports, self.weak_imports = [], set(), 0
        d = self.data
        dynsym = next((s for s in self.sh if s[1] == SHT_DYNSYM), None)
        if not dynsym:
            return
        strs = self.sh[dynsym[6]][4]
        versym = next((s for s in self.sh if s[1] == SHT_VERSYM), None)
        vernames = {}
        verneed = next((s for s in self.sh if s[1] == SHT_VERNEED), None)
        if verneed:
            vstr, off = self.sh[verneed[6]][4], verneed[4]
            for _ in range(verneed[7]):
                _, cnt, _file, aux, nxt = struct.unpack_from('<HHIII', d, off)
                a = off + aux
                for _ in range(cnt):
                    _, _, other, name, anext = struct.unpack_from('<IHHII', d, a)
                    vernames[other] = cstr(d, vstr + name)
                    a += anext
                off += nxt
        for i in range(1, dynsym[5] // 24):
            name, info, _, shndx, _, _ = struct.unpack_from('<IBBHQQ', d, dynsym[4] + i * 24)
            sym = cstr(d, strs + name)
            if not sym:
                continue
            if shndx == 0:
                ver = None
                if versym:
                    ver = vernames.get(struct.unpack_from('<H', d, versym[4] + i * 2)[0] & 0x7fff)
                self.imports.append((sym, ver))
                self.weak_imports += (info >> 4) == 2
            elif (info >> 4) in (1, 2, 10):               # GLOBAL, WEAK, GNU_UNIQUE
                self.exports.add(sym)

    def _relocs(self):
        self.relocs = Counter()
        for s in self.sh:
            if s[1] == SHT_RELA:
                for off in range(s[4], s[4] + s[5], 24):
                    t = struct.unpack_from('<Q', self.data, off + 8)[0] & 0xffffffff
                    self.relocs[RELOC.get(t, 'type%d' % t)] += 1
            elif s[1] == SHT_RELR:
                self.relocs['RELR words'] += s[5] // 8

    def _code(self):
        """Count the instructions a Linux-ABI relink has to veneer."""
        self.insn = Counter()
        for s in self.sh:
            if s[1] != 1 or not s[2] & 0x4:               # PROGBITS + EXECINSTR
                continue
            words = array.array('I')
            words.frombytes(self.data[s[4]:s[4] + (s[5] & ~3)])
            self.insn['total'] += len(words)
            for w in words:
                top = w >> 20
                if top == 0xd53:
                    if w & 0xffffffe0 == 0xd53bd040:
                        self.insn['mrs tpidr_el0'] += 1
                    elif w & 0xffffffe0 == 0xd53b0020:
                        self.insn['mrs ctr_el0'] += 1
                elif top == 0xd51 and w & 0xffffffe0 == 0xd51bd040:
                    self.insn['msr tpidr_el0'] += 1
                elif w & 0xffe0001f == 0xd4000001:
                    self.insn['svc'] += 1

    @property
    def kind(self):
        if self.e_type == 2:
            return 'EXEC (non-PIE)'
        if self.e_type == 3:
            return 'PIE' if (self.interp or self.flags1 & DF_1_PIE) else 'DSO'
        return 'type %d' % self.e_type


def walk(roots):
    for root in roots:
        if os.path.isfile(root):
            yield root
        for dp, _, files in os.walk(root):
            for f in sorted(files):
                yield os.path.join(dp, f)


def version_key(v):
    return tuple(int(x) for x in re.findall(r'\d+', v))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('roots', nargs='+')
    ap.add_argument('--runtime', action='append', default=[])
    ap.add_argument('--arch-dir', default='arm64',
                    help='only audit ELFs whose path contains this (default: arm64); "" for all')
    ap.add_argument('-o', '--output')
    args = ap.parse_args()

    elfs, empty = [], []
    for p in walk(args.roots):
        if args.arch_dir and args.arch_dir not in p:
            continue
        if os.path.islink(p):
            continue
        if os.path.getsize(p) == 0 and '.so' in os.path.basename(p):
            empty.append(p)
            continue
        with open(p, 'rb') as f:
            if f.read(4) != b'\x7fELF':
                continue
        e = Elf(p)
        if e.ok:
            elfs.append(e)

    # Who can satisfy an import or a DT_NEEDED: the audited tree first, then the runtime.
    provider, have = {}, set()
    for e in elfs:
        name = e.soname or os.path.basename(e.path)
        have.update({name, os.path.basename(e.path)})
        for s in e.exports:
            provider.setdefault(s, name)
    rt_files = {}
    for rt in args.runtime:
        for p in walk([rt]):
            rt_files.setdefault(os.path.basename(p), p)
    rt_loaded = {}

    def runtime_lib(name):
        if name not in rt_loaded:
            p = rt_files.get(name)
            e = Elf(os.path.realpath(p)) if p and os.path.getsize(os.path.realpath(p)) else None
            rt_loaded[name] = e if e and e.ok else None
        return rt_loaded[name]

    # The DT_NEEDED closure, so runtime exports are only credited to libraries
    # that would actually be loaded.
    closure, todo = [], [n for e in elfs for n in e.needed] + [os.path.basename(p) for p in empty]
    missing = set()
    while todo:
        n = todo.pop()
        if n in have or n in closure or n in missing:
            continue
        lib = runtime_lib(n)
        if not lib:
            missing.add(n)
            continue
        closure.append(n)
        todo += lib.needed
    for n in closure:
        for s in runtime_lib(n).exports:
            provider.setdefault(s, n)

    out = []
    w = out.append
    w('# ELF audit\n')
    w('Roots: %s  \nRuntime: %s\n' % (', '.join(args.roots), ', '.join(args.runtime) or '-'))
    w('%d ARM64 ELF files audited, %d empty placeholder libraries.\n' % (len(elfs), len(empty)))

    w('## Files\n')
    w('| file | kind | size | p_align | first vaddr | TLS memsz | static TLS | NEEDED | imports | sections |')
    w('|---|---|---:|---:|---:|---:|---|---:|---:|---|')
    common = os.path.commonpath([e.path for e in elfs] or ['.'])
    if os.path.isfile(common):
        common = os.path.dirname(common)
    rel = lambda p: os.path.relpath(p, common)
    for e in elfs:
        w('| %s | %s | %.1f MB | 0x%x | 0x%x | %s | %s | %d | %d | %s |' % (
            rel(e.path), e.kind, len(e.data) / 1e6, e.p_align, e.first_vaddr or 0,
            e.tls[0] if e.tls else '-', 'yes' if e.flags & DF_STATIC_TLS else '-',
            len(e.needed), len(e.imports), 'yes' if e.sh else '**none**'))

    w('\n## Loader requirements\n')
    execs = [e for e in elfs if e.kind != 'DSO']
    w('- Executables: ' + (', '.join('`%s` (%s, interp `%s`, entry 0x%x)' % (
        rel(e.path), e.kind, e.interp, e.e_entry) for e in execs) or 'none found'))
    w('- Largest `p_align`: 0x%x' % max((e.p_align for e in elfs), default=0))
    w('- Files with PT_TLS: %d, of which DF_STATIC_TLS: %d' % (
        sum(1 for e in elfs if e.tls), sum(1 for e in elfs if e.flags & DF_STATIC_TLS)))
    total = Counter()
    for e in elfs:
        total.update(e.relocs)
    w('\n| relocation | count | applied by kl_image.c today |')
    w('|---|---:|---|')
    for name, n in total.most_common():
        w('| %s | %d | %s |' % (name, n, 'yes' if name in RELOC_HANDLED or name.startswith('RELR') else '**no**'))

    w('\n## Instructions needing a veneer under a Linux ABI\n')
    w('| file | instructions | mrs tpidr_el0 | msr tpidr_el0 | svc | mrs ctr_el0 |')
    w('|---|---:|---:|---:|---:|---:|')
    isum = Counter()
    for e in elfs:
        isum.update(e.insn)
        if e.insn['mrs tpidr_el0'] or e.insn['svc'] or e.insn['msr tpidr_el0'] or e.insn['mrs ctr_el0']:
            w('| %s | %d | %d | %d | %d | %d |' % (rel(e.path), e.insn['total'], e.insn['mrs tpidr_el0'],
                                                e.insn['msr tpidr_el0'], e.insn['svc'], e.insn['mrs ctr_el0']))
    w('| **total** | %d | %d | %d | %d | %d |' % (isum['total'], isum['mrs tpidr_el0'],
                                                 isum['msr tpidr_el0'], isum['svc'], isum['mrs ctr_el0']))
    w('\nx18 sites are not counted here: run `tools/check_x18.py build/t_x18 <libs>`.')

    w('\n## Dependencies\n')
    needed = Counter(n for e in elfs for n in e.needed)
    w('| DT_NEEDED | wanted by | supplied by |')
    w('|---|---:|---|')
    for n, c in sorted(needed.items()):
        src = 'game tree' if n in have else 'runtime' if runtime_lib(n) else '**missing**'
        w('| %s | %d | %s |' % (n, c, src))
    if empty:
        w('\nEmpty placeholders (the runtime must supply them): ' +
          ', '.join('`%s`%s' % (os.path.basename(p), '' if runtime_lib(os.path.basename(p)) else ' (**not in runtime**)')
                    for p in empty))
    w('\nRuntime libraries pulled in by the closure (%d): %s' % (len(closure), ', '.join(sorted(closure))))
    if missing:
        w('\n**Not found anywhere:** ' + ', '.join(sorted(missing)))

    w('\n## Imports\n')
    by_provider, versions, unresolved = defaultdict(set), defaultdict(set), Counter()
    for e in elfs:
        for sym, ver in e.imports:
            p = provider.get(sym)
            if p in have:
                continue
            if p is None:
                unresolved[sym] += 1
            else:
                by_provider[p].add(sym)
            if ver:
                versions[ver.split('_')[0]].add(ver)
    w('| provider | distinct symbols |')
    w('|---|---:|')
    for p, syms in sorted(by_provider.items(), key=lambda kv: -len(kv[1])):
        w('| %s | %d |' % (p, len(syms)))
    w('| *(no provider found)* | %d |' % len(unresolved))
    for fam, vs in sorted(versions.items()):
        w('\n- Highest `%s` version required: `%s`' % (fam, max(vs, key=version_key)))
    if unresolved:
        w('\nUnresolved (first 60 of %d): %s' % (len(unresolved), ', '.join('`%s`' % s for s in sorted(unresolved)[:60])))

    w('\n## API probes\n')
    for label, pat in PROBES:
        rx, hits = re.compile(pat), defaultdict(set)
        for e in elfs:
            for m in rx.finditer(e.data):
                hits[m.group().decode('latin-1')].add(rel(e.path))
        w('### %s (%d distinct)\n' % (label, len(hits)))
        for s in sorted(hits)[:120]:
            files = sorted(hits[s])
            w('- `%s` — %s' % (s, ', '.join(files[:3]) + (' +%d' % (len(files) - 3) if len(files) > 3 else '')))
        w('')

    text = '\n'.join(out) + '\n'
    if args.output:
        with open(args.output, 'w') as f:
            f.write(text)
        print('wrote %s (%d files audited)' % (args.output, len(elfs)))
    else:
        sys.stdout.write(text)


if __name__ == '__main__':
    main()
