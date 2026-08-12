#!/usr/bin/env python3
"""Name the `lib+0xoffset` addresses in a klepton log.

Why this exists: the runtime prints callers as `libOculusXRPlugin.so+0xf294`
because kl_image only parses `.dynsym`, and the interesting guest libraries keep
a `.symtab` that it never looks at (libOculusXRPlugin.so has 1009 symbols against
its handful of dynamic ones). So the log has the address and the library has the
name, and joining them is the difference between reading

    [ovrp+] 1 ovrp_SetupDisplayObjects2 <- libOculusXRPlugin.so+0x12014

and reading

    ... <- libOculusXRPlugin.so+0x12014 [OculusDisplayProvider::CreateMobileDisplayObjects+0x74]

which is what turned "the display subsystem stops after one frame" into "it stops
because CreateMobileDisplayObjects reported failure" in a single pass. Doing the
join here rather than in the shim keeps a build-time symbol table out of the
runtime, and works on logs captured weeks ago.

    tools/symof.py <log>            # or on a pipe
    tools/symof.py <log> --libs <dir>

Nearest-preceding-symbol, so an address inside a function reports that function
plus an offset. A library with no symbols is left exactly as it was.
"""
import argparse
import bisect
import os
import re
import subprocess
import sys

PAT = re.compile(r"(lib[\w.+-]*\.so)\+0x([0-9a-fA-F]+)")


def load(path):
    """(sorted addrs, names) from `nm`, or ([], []) if there is nothing to read."""
    if not os.path.exists(path):
        return [], []
    try:
        out = subprocess.run(["nm", "--defined-only", path],
                             capture_output=True, text=True).stdout
    except OSError:
        return [], []
    syms = []
    for line in out.splitlines():
        parts = line.split(None, 2)
        if len(parts) != 3 or not parts[0].strip():
            continue
        try:
            syms.append((int(parts[0], 16), parts[2]))
        except ValueError:
            pass
    syms.sort()
    return [s[0] for s in syms], [s[1] for s in syms]


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("log", nargs="?", help="log file (default: stdin)")
    ap.add_argument("--libs", default="beatsaber/lib/arm64-v8a",
                    help="directory holding the guest .so files")
    args = ap.parse_args()

    cache = {}

    def name(lib, off):
        if lib not in cache:
            cache[lib] = load(os.path.join(args.libs, lib))
        addrs, names = cache[lib]
        if not addrs:
            return None
        i = bisect.bisect_right(addrs, off) - 1
        if i < 0:
            return None
        return "%s+0x%x" % (names[i], off - addrs[i])

    def sub(m):
        n = name(m.group(1), int(m.group(2), 16))
        return m.group(0) + (" [%s]" % n if n else "")

    src = open(args.log, errors="replace") if args.log else sys.stdin
    for line in src:
        sys.stdout.write(PAT.sub(sub, line))


if __name__ == "__main__":
    main()
