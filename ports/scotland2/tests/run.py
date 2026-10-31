"""Exercise real upstream Scotland2 core with a mocked image-loader boundary."""
from pathlib import Path
import struct
import json
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[3]
OUT = ROOT / "build/scotland2/macosx"


def elf(needed):
    strings = b"\0"
    dynamic = b""
    for name in needed:
        dynamic += struct.pack("<qQ", 1, len(strings))
        strings += name.encode() + b"\0"
    dynamic += struct.pack("<qQ", 0, 0)
    dynoff = 64 + 3 * 64
    stroff = dynoff + len(dynamic)
    header = struct.pack("<16sHHIQQQIHHHHHH", b"\x7fELF\x02\x01\x01" + b"\0" * 9,
                         3, 183, 1, 0, 0, 64, 0, 64, 56, 0, 64, 3, 0)
    sections = bytes(64)
    sections += struct.pack("<IIQQQQIIQQ", 0, 6, 0, 0, dynoff, len(dynamic), 2, 0, 8, 16)
    sections += struct.pack("<IIQQQQIIQQ", 0, 3, 0, 0, stroff, len(strings), 0, 0, 1, 0)
    return header + sections + dynamic + strings


subprocess.run(["bash", "ports/scotland2/build.sh", "macosx"], cwd=ROOT, check=True)
exe = OUT / "test-core"
subprocess.run(["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-g",
                "-DSL2_KLEPTON", "-Iports/scotland2/include", "-Iports/scotland2/upstream/shared",
                "ports/scotland2/tests/core.cpp", str(OUT / "libscotland2-core.a"), "-o", str(exe)], cwd=ROOT, check=True)
for scenario in ["success", "open-failure", "foreign-export", "malformed"]:
    with tempfile.TemporaryDirectory(prefix="scotland2-core-") as tmp:
        root = Path(tmp)
        for phase in ["libs", "early_mods", "mods", "shims"]:
            (root / phase).mkdir()
        (root / "libs/libzsupport.so").write_bytes(elf([]))
        (root / "libs/libaconsumer.so").write_bytes(elf(["libzsupport.so"]))
        (root / "early_mods/libearly.so").write_bytes(elf(["libaconsumer.so"]))
        (root / "mods/liblate.so").write_bytes(elf(["libaconsumer.so"]))
        if scenario == "malformed":
            (root / "libs/libaconsumer.so").write_bytes(b"\x7fELF")
        result = subprocess.run([str(exe), scenario, str(root)], capture_output=True, text=True)
        (OUT / (scenario + ".log")).write_text(result.stdout + result.stderr)
        if result.returncode:
            print(result.stdout + result.stderr)
            raise RuntimeError(f"{scenario} failed with {result.returncode}")
        print(result.stdout.strip())

parser = OUT / "test-parser"
subprocess.run(["xcrun", "clang++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-Iports/scotland2/include", "ports/scotland2/tests/parser.cpp",
                str(OUT / "libscotland2-core.a"), "-o", str(parser)], cwd=ROOT, check=True)
inventory = json.loads((ROOT / "port-results/coremods/coremods-lock.json").read_text())
for binary in [b for b in inventory["binaries"] if b["mod"] != "FakeId"]:
    result = subprocess.run([str(parser), str(ROOT / "port-results/coremods" / binary["file"])],
                            capture_output=True, text=True, check=True)
    assert result.stdout.splitlines() == binary["needed"], binary["file"]
print(f"PASS dependency parser matches the independent audit for all {len([b for b in inventory['binaries'] if b['mod']!='FakeId'])} actual mod libraries")
bad = []
valid = elf(["libc.so"])
bad.extend([b"", valid[:63], b"Not ELF" + bytes(300)])
for offset, fmt, value in [(4, "B", 1), (5, "B", 2), (18, "H", 62), (40, "Q", 2**64-1),
                          (64+64+24, "Q", 2**64-1), (64+64+40, "I", 999)]:
    data = bytearray(valid)
    struct.pack_into("<" + fmt, data, offset, value)
    bad.append(data)
bad.extend([elf(["../libc.so"]), valid[:-1]])
with tempfile.TemporaryDirectory(prefix="scotland2-parser-") as tmp:
    path = Path(tmp) / "bad.so"
    for data in bad:
        path.write_bytes(data)
        result = subprocess.run([str(parser), str(path)], capture_output=True)
        assert result.returncode == 1 and result.stderr, (result, data)
print(f"PASS dependency parser rejects {len(bad)} malformed/truncated/unsupported metadata inputs")
