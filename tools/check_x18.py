#!/usr/bin/env python3
"""Cross-check runtime/kl_x18.c against objdump over a guest library's .text.

A decoder bug here is a silent wrong answer — the worst failure class in this
project — so the decoder is not trusted on its own reasoning. This checks it
against an independent disassembler in both directions:

  MISS          objdump prints an x18/w18 operand and the decoder did not find
                one. The site would be left broken, i.e. the bug persists.
  FALSE ALARM   the decoder claims an x18 field where objdump shows none.
                Substituting there would corrupt an unrelated instruction.
  BAD SUBST     the rewritten word does not disassemble to the original text
                with x18->x9. The field map is wrong for that encoding.
  REFUSED       the decoder declined an encoding that really does use x18.
                Not dangerous — the loader will refuse to patch it and say so —
                but it is work remaining, so they are listed by encoding.

DATA WORDS ARE NOT GRADED, and that is the seventh column t_x18 prints. An
executable section can hold constant tables (libcrypto.so keeps 148 KB
of them in .text, libmain.so on the Steam Link side likewise), objdump decodes
those bytes as whatever encoding they spell, and the loader patches none of
them. Grading them makes the gate measure the disassembler against a random
number generator: 327 "false alarms" on undecodable words and 68 "refusals" of
SVE/SME/MTE encodings no AArch64 BoringSSL build emits, all inside one 148 KB
block, none of them reachable. What IS graded is every word the loader would
act on. The excluded counts are printed per library rather than dropped: they
are how a data detector that starts swallowing real code shows up.

Substitution is checked by assembling the original and rewritten words into two
objects at identical offsets, so pc-relative operands render identically and any
difference is the register itself.

Usage: tools/check_x18.py build/t_x18 beatsaber/lib/arm64-v8a/libunity.so ...
"""
import re
import subprocess
import sys
import tempfile
from collections import Counter
from pathlib import Path

OBJDUMP = "/usr/bin/objdump"
TEST_REG = 9

# `x18` as an operand, not the tail of a hex literal (#0x18) or a vector name.
X18 = re.compile(r"(?<![0-9A-Za-z_.$])([wx])18(?![0-9A-Za-z_])")
INSN = re.compile(r"^\s*([0-9a-f]+):\s+(.*?)\s*$")


def disassemble(path, extra=()):
    """-> {address: normalised instruction text}"""
    out = subprocess.run([OBJDUMP, "-d", "--no-show-raw-insn", *extra, str(path)],
                         capture_output=True, text=True, errors="replace").stdout
    insns = {}
    for line in out.splitlines():
        m = INSN.match(line)
        if not m:
            continue
        text = " ".join(m.group(2).split())
        if not text:
            continue
        insns[int(m.group(1), 16)] = text
    return insns


def disassemble_words(words):
    """Assemble raw words at 4-byte offsets and disassemble them back."""
    with tempfile.TemporaryDirectory() as d:
        s, o = Path(d) / "w.s", Path(d) / "w.o"
        s.write_text(".text\n" + "".join(f".long 0x{w:08x}\n" for w in words))
        r = subprocess.run(["clang", "-c", "-arch", "arm64", "-o", str(o), str(s)],
                           capture_output=True, text=True)
        if r.returncode != 0:
            sys.exit(f"assembling probe words failed:\n{r.stderr}")
        return disassemble(o)


def check(t_x18, lib):
    decoded = {}
    proc = subprocess.run([str(t_x18), str(lib)], capture_output=True, text=True)
    if proc.returncode != 0:
        sys.exit(f"{t_x18} failed:\n{proc.stderr}")
    for line in proc.stdout.splitlines():
        va, word, subst, ok, nfields, roles, data = line.split()
        decoded[int(va, 16)] = (int(word, 16), int(subst, 16),
                                int(ok), int(nfields), int(data))

    real = disassemble(lib)
    misses, false_alarms, refused = [], [], []
    sites = []
    skipped = Counter()

    for addr, text in real.items():
        entry = decoded.get(addr)
        objdump_x18 = bool(X18.search(text))
        if entry is None:
            # Not printed by t_x18 at all: either the decoder had nothing to say
            # (ok, no fields) or the word is inside a range the loader skips
            # wholesale. Only an x18 operand makes that interesting.
            if objdump_x18:
                misses.append((addr, text, None))
            continue
        word, subst, ok, nfields, data = entry
        if data:
            # A word the loader will not patch. Count the disagreement, do not
            # grade it — see the module docstring.
            if not ok and objdump_x18:
                skipped["refusal"] += 1
            elif nfields and not objdump_x18:
                skipped["false alarm"] += 1
            elif objdump_x18 and not nfields:
                skipped["miss"] += 1
            skipped["total"] += 1
            continue
        if not ok:
            if objdump_x18:
                refused.append((addr, word, text))
            continue
        if nfields and not objdump_x18:
            false_alarms.append((addr, text, word))
        elif objdump_x18 and not nfields:
            misses.append((addr, text, word))
        if nfields:
            sites.append((addr, word, subst, text))

    # Substitution check: same offsets in both objects, so only the register moves.
    bad_subst = []
    if sites:
        before = disassemble_words([s[1] for s in sites])
        after = disassemble_words([s[2] for s in sites])
        for i, (addr, word, subst, _text) in enumerate(sites):
            off = i * 4
            got, orig = after.get(off), before.get(off)
            if orig is None or got is None:
                bad_subst.append((addr, word, subst, orig, got))
                continue
            want = X18.sub(lambda m: f"{m.group(1)}{TEST_REG}", orig)
            if got != want:
                bad_subst.append((addr, word, subst, want, got))

    name = Path(lib).name
    print(f"=== {name}: {len(real)} instructions, {len(sites)} x18 sites ===")
    if skipped["total"]:
        detail = ", ".join(f"{skipped[k]} {k}" for k in ("miss", "false alarm", "refusal")
                           if skipped[k]) or "no disagreement with objdump"
        print(f"  {skipped['total']} data word(s) inside executable "
              f"sections not graded — {detail}")
    ok = True

    def report(label, rows, fmt, limit=12):
        nonlocal ok
        if not rows:
            return
        ok = False
        print(f"  {label}: {len(rows)}")
        for row in rows[:limit]:
            print(f"    {fmt(row)}")
        if len(rows) > limit:
            print(f"    ... and {len(rows) - limit} more")

    report("MISS", misses, lambda r: f"{r[0]:x}  {r[1]}")
    report("FALSE ALARM", false_alarms, lambda r: f"{r[0]:x}  {r[2]:08x}  {r[1]}")
    report("BAD SUBST", bad_subst,
           lambda r: f"{r[0]:x}  {r[1]:08x}->{r[2]:08x}  want {r[3]!r} got {r[4]!r}")

    if refused:
        ok = False
        print(f"  REFUSED (real x18 sites the decoder declined): {len(refused)}")
        for mnemonic, n in Counter(r[2].split()[0] for r in refused).most_common(12):
            example = next(r for r in refused if r[2].split()[0] == mnemonic)
            print(f"    {n:6d}  {mnemonic:<10} e.g. {example[1]:08x}  {example[2]}")

    if ok:
        print("  clean: no misses, no false alarms, every substitution round-trips")
    return ok


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    t_x18, libs = sys.argv[1], sys.argv[2:]
    if not all(check(t_x18, lib) for lib in libs):
        sys.exit(1)
    print("\nall libraries clean")


if __name__ == "__main__":
    main()
