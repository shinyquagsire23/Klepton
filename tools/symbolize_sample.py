#!/usr/bin/env python3
"""Rewrite `sample` output so guest frames have names.

`sample <pid>` reports every guest frame as `??? (in <unknown binary>)`, because
the guest images are mmap'd by us and dyld has never heard of them. That turns
the one diagnostic that keeps paying off (CLAUDE.md, "three diagnostics") into a
wall of hex the moment the target has fourteen libraries.

The load addresses are already printed by m_slink/t_boot's phase 1 as
`  <soname>  @0x...`, so this joins the two: read the bases out of the run's
log, look each unknown address up in the owning library's .dynsym, and print
`<lib>`<nearest symbol>+0xoff`.

  ./tools/symbolize_sample.py <sample-file> <run-log> [libdir]

The nearest DEFINED dynamic symbol is a lower bound on the truth: these guests
are stripped to .dynsym, so a static function shows up as "the exported symbol
before it". A name plus a large offset means "somewhere after that export",
which is still enough to name the subsystem — do not read the offset as
meaningful once it is over a few KB.
"""
import bisect
import os
import re
import subprocess
import sys


def symbols(path):
    """(sorted addrs, demangled names) of the defined text symbols in an ELF .so."""
    out = subprocess.run(["nm", "-D", "--defined-only", path],
                         capture_output=True, text=True).stdout
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) == 3 and parts[1] in "tTwWiI":
            try:
                syms.append((int(parts[0], 16), parts[2].split("@@")[0]))
            except ValueError:
                pass
    syms.sort()
    if not syms:
        return [], []
    names = subprocess.run(["c++filt"], input="\n".join(s[1] for s in syms),
                           capture_output=True, text=True).stdout.splitlines()
    return [s[0] for s in syms], names


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    sample_file, log_file = sys.argv[1], sys.argv[2]
    libdir = sys.argv[3] if len(sys.argv) > 3 else None

    # `  libfoo.so   @0x104f41000   1.27 MB ...` — phase 1's report_image line.
    images = []          # (base, span, soname, path)
    log = open(log_file, "rb").read().decode("utf-8", "replace")
    for m in re.finditer(r"^\s+(\S+\.so)\s+@0x([0-9a-f]+)\s+([\d.]+) MB", log, re.M):
        soname, base, mb = m.group(1), int(m.group(2), 16), float(m.group(3))
        images.append([base, int(mb * 1048576) + 65536, soname, None])
    if not images:
        sys.exit(f"{log_file}: no `<lib> @0x...` lines — was this run's phase 1 logged?")

    if not libdir:
        # The log names the tree it loaded; fall back to the usual place.
        m = re.search(r"front door \([^)]*\), (\S+)", log)
        libdir = m.group(1) if m else "steamlink-vr/lib/arm64-v8a"
    for img in images:
        img[3] = os.path.join(libdir, img[2])

    images.sort()
    bases = [i[0] for i in images]
    cache = {}

    def name_for(addr):
        i = bisect.bisect_right(bases, addr) - 1
        if i < 0:
            return None
        base, span, soname, path = images[i]
        if addr >= base + span or not os.path.exists(path):
            return None
        if path not in cache:
            cache[path] = symbols(path)
        addrs, names = cache[path]
        off = addr - base
        j = bisect.bisect_right(addrs, off) - 1
        if j < 0:
            return f"{soname}+0x{off:x}"
        return f"{soname}`{names[j]}+0x{off - addrs[j]:x}"

    pat = re.compile(r"\?\?\?\s+\(in <unknown binary>\)\s+\[0x([0-9a-f]+)\]")
    for line in open(sample_file, "rb").read().decode("utf-8", "replace").splitlines():
        m = pat.search(line)
        if m:
            got = name_for(int(m.group(1), 16))
            if got:
                line = line[:m.start()] + got + f"  [0x{m.group(1)}]"
        print(line)


if __name__ == "__main__":
    main()
