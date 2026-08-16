#!/usr/bin/env python3
"""Do our OVRPlugin implementations agree with the REAL plugin about pointers?

The APK ships the libOVRPlugin.so we replace. We never load a byte of it — it
needs Quest system libraries that do not exist here (PLANNING §3.1) — but it is
the authority on the one thing about each entry point we cannot derive: whether
an argument is an out-parameter, and which one.

That question has now cost two separate arcs. Trap 10 was its return-value half
(`ovrpResult` success is 0, `ovrpBool` success is 1, and answering one where the
other is read is a failure code the guest carries far away). This is its
argument half, and it is worse, because there is no value to get wrong:

    ovrp_GetVersion    is `const char *ovrp_GetVersion(void)`
    ovrp_GetVersion2   is `ovrpResult ovrp_GetVersion2(const char **)`

— the un-suffixed one is a WRAPPER around the 2-form, not an older spelling of
it. Implementing the 2-form as a scalar string return leaves the caller's out
pointer untouched, and a caller that then strlen()s it dies at 0x0 in
_platform_strlen with one guest frame above it, naming neither the function nor
the subsystem. It read correctly for the whole project because every caller that
mattered went through the C# `ovrp_GetVersion`; Beat Saber 1.40's libhaptics_sdk
calls the 2-form directly from native code.

So: disassemble each entry point we implement, find the arguments it refuses as
NULL, and compare against our own handler's parameter list.

**How an out-parameter is recognised, and why the obvious test is wrong.** Every
one of these functions begins by loading a singleton and `cbz`-ing it, which
looks exactly like an argument check. The two are told apart by the code they
branch to: a NULL ARGUMENT answers ovrpFailure_InvalidParameter (-1001), a
missing singleton answers ovrpFailure_NotInitialized (-1002). Testing the `cbz`
alone reports ovrp_SetSystemDisplayFrequency — which takes a float — as taking a
pointer.

    python3 tools/ovrp_abi.py [<libOVRPlugin.so>] [<kl_ovrp.c>]

Exit 1 on a disagreement, 0 when clean, and 0 with a note when the real plugin
is not in the tree (Steam Link has none; neither does a guest unpacked without
its libraries).
"""
import re
import subprocess
import sys

INVALID_PARAM = "#-0x3e9"      # -1001, ovrpFailure_InvalidParameter


def real_symbols(so):
    out = subprocess.run(["nm", "-D", so], capture_output=True, text=True).stdout
    syms = {}
    for line in out.splitlines():
        p = line.split()
        if len(p) == 3 and p[1] == "T" and p[2].startswith("ovrp_"):
            syms[p[2]] = int(p[0], 16)
    return syms


def out_param_regs(so, addr, span=0x60):
    """Which xN the real function refuses as NULL with -1001."""
    d = subprocess.run(
        ["objdump", "-d", f"--start-address={addr}", f"--stop-address={addr + span}", so],
        capture_output=True, text=True).stdout
    body = d.split(">:", 1)[-1] if ">:" in d else d
    regs = set()
    for m in re.finditer(r"cbz\s+x(\d+),\s+0x([0-9a-f]+)", body):
        reg, target = int(m.group(1)), m.group(2)
        # ...and the branch target answers -1001 rather than -1002.
        if re.search(r"\n\s*" + target + r":\s+\S+\s+mov\s+w0, " + re.escape(INVALID_PARAM), body):
            regs.add(reg)
    return regs


def ours(src):
    """{entry point name: (handler, parameter list)} from the impl table."""
    table = re.search(r"g_ovrp_impl\[\]\s*=\s*\{(.*?)\n\};", src, re.S)
    if not table:
        sys.exit("!! could not find g_ovrp_impl[] — has kl_ovrp.c been restructured?")
    pairs = re.findall(r'\{"(ovrp_[A-Za-z0-9_]+)",\s*\(void \*\)([A-Za-z0-9_]+)\}', table.group(1))
    sigs = {}
    for m in re.finditer(r"^(?:static\s+)?(?:const char \*|uint64_t|int64_t|void|float)\s*\*?\s*"
                         r"([A-Za-z0-9_]+)\s*\(([^)]*)\)\s*\{", src, re.M):
        sigs[m.group(1)] = m.group(2).strip()
    return {name: (fn, sigs.get(fn)) for name, fn in pairs}


def main(argv):
    so  = argv[1] if len(argv) > 1 else "beatsaber/lib/arm64-v8a/libOVRPlugin.so"
    csrc = argv[2] if len(argv) > 2 else "runtime/xr/kl_ovrp.c"
    syms = real_symbols(so)
    if not syms:
        print(f"  ovrp ABI: no real libOVRPlugin.so at {so} — nothing to compare against")
        return 0
    mine = ours(open(csrc).read())
    checked = bad = 0
    for name, (fn, params) in sorted(mine.items()):
        if name not in syms:
            continue                      # ours only — Unity's plugin interface, etc.
        checked += 1
        regs = out_param_regs(so, syms[name])
        if not regs:
            continue
        if params is None:
            print(f"  ?? {name}: could not read {fn}'s parameter list")
            continue
        # An out-parameter in xN needs an argument at that position in ours, and
        # the only thing checkable from the C text is that a pointer is there at
        # all — floats travel in v registers, so a `float, float*` handler still
        # has its pointer in x0.
        if "*" not in params:
            bad += 1
            print(f"  !! {name}: the real plugin refuses a NULL x{min(regs)} "
                  f"(-1001), but {fn}({params}) takes no pointer.\n"
                  f"     An out-parameter we never write is a strlen/read of "
                  f"uninitialised caller memory, far from here.")
    print(f"  ovrp ABI: {checked} implemented entry point(s) checked against the "
          f"real plugin, {bad} disagreement(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
