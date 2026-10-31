#!/usr/bin/env python3
"""Half-Life: Alyx's Vulkan pipeline list, rebuilt from the player's own copy of the game.

A pipeline list is a Fossilize archive (.foz): shader modules plus the Vulkan state that makes pipelines of them.
Pre-compiling it before play keeps Metal's 300-600 ms first-use shader compiles out of the game. The shader modules
are the game's own SPIR-V and are not ours to pass on, but each is a byte-identical slice of a shader file in the
player's copy. So what is passed on is a RECIPE: the archive with every shader module replaced by where to find it
(pack, .vcs file, static combo, offset, size, SHA-256). This script turns a recipe back into the archive.

    alyx_pipelines.py build <recipe> <game dir> <out.foz>    rebuild the archive from the game's shader packs
    alyx_pipelines.py check <recipe>                         a recipe must carry no shader code
    alyx_pipelines.py info  <recipe or .foz>                 what is in it

<game dir> is the depot's game/ folder (core/ and hlvr/, each with shaders_vulkan_dir.vpk).

The recipe format and this approach are the Glovebox project's (github.com/glovebox-vr/glovebox, engine/pipelines.py
and engine/gb/{vpk,vcs,foz}.py, MIT licence, (c) 2026 the Glovebox contributors); this file is those parts in one,
reading the same recipes.
"""
import collections
import hashlib
import json
import lzma
import os
import struct
import sys
import zlib
from multiprocessing import Pool

REF_MAGIC = b"GBREF"
SPIRV_MAGIC = b"\x03\x02\x23\x07"
TAG_NAMES = {"00": "application info", "01": "samplers", "02": "set layouts", "03": "pipeline layouts",
             "04": "shader modules", "05": "render passes", "06": "graphics pipelines", "07": "compute pipelines",
             "08": "application links"}


class VPK:
    """Read-only access to a Valve pack (VPK v1/v2)."""

    def __init__(self, dir_path):
        if not dir_path.endswith("_dir.vpk"):
            raise ValueError(f"{dir_path}: not a *_dir.vpk")
        self.base = dir_path[: -len("_dir.vpk")]
        d = open(dir_path, "rb").read()
        sig, ver, tree = struct.unpack_from("<III", d, 0)
        if sig != 0x55AA1234:
            raise ValueError(f"{dir_path}: bad VPK signature")
        hdr = 12 if ver == 1 else 28
        self._inline_base = hdr + tree
        self._dir = d
        self.entries = {}
        off = hdr

        def rs():
            nonlocal off
            e = d.index(b"\0", off)
            s = d[off:e].decode("latin1")
            off = e + 1
            return s

        while True:
            ext = rs()
            if not ext:
                break
            while True:
                path = rs()
                if not path:
                    break
                while True:
                    name = rs()
                    if not name:
                        break
                    _crc, pre, arc, aoff, alen, _term = struct.unpack_from("<IHHIIH", d, off)
                    off += 18
                    full = (f"{path}/" if path.strip() else "") + f"{name}.{ext}"
                    self.entries[full] = (arc, aoff, alen, off, pre)
                    off += pre

    def read(self, name):
        arc, aoff, alen, poff, pre = self.entries[name]
        data = self._dir[poff : poff + pre]
        if alen:
            if arc == 0x7FFF:
                start = self._inline_base + aoff
                data += self._dir[start : start + alen]
            else:
                with open(f"{self.base}_{arc:03d}.vpk", "rb") as f:
                    f.seek(aoff)
                    data += f.read(alen)
        return data


def vcs_static_table(d):
    """{static combo id: file offset}, or None for a file without code. The table sits just before a field holding
    the file's length and is found by its shape: n, n int64 ids, n int32 offsets."""
    key = struct.pack("<i", len(d))
    q = d.find(key)
    while q >= 0:
        for n in range(1, 50000):
            p = q - 12 * n - 4
            if p < 0:
                break
            if struct.unpack_from("<i", d, p)[0] == n and struct.unpack_from("<i", d, p + 4 + 8 * n)[0] == q + 4:
                ids = struct.unpack_from(f"<{n}q", d, p + 4)
                offs = struct.unpack_from(f"<{n}i", d, p + 4 + 8 * n)
                return dict(zip(ids, offs))
        q = d.find(key, q + 1)
    return None


def vcs_static_blob(d, off):
    """One static combo: a raw LZMA1 stream behind a small header with the magic "LZMA"."""
    _, magic, usize, csize = struct.unpack_from("<4i", d, off)
    if magic != 0x414D5A4C:
        raise ValueError(f"static combo at {off}: not LZMA ({magic:#x})")
    props = d[off + 16 : off + 21]
    lc, lp, pb = props[0] % 9, (props[0] // 9) % 5, props[0] // 45
    dict_size = struct.unpack_from("<I", props, 1)[0]
    dec = lzma.LZMADecompressor(lzma.FORMAT_RAW,
                                filters=[{"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": lp, "pb": pb}])
    return dec.decompress(d[off + 21 : off + 21 + csize], max_length=usize)


def foz_read(path):
    """(16-byte file header, [(name, format, crc, size, stored bytes)]). A torn tail is dropped, as Fossilize does."""
    d = open(path, "rb").read()
    out, off = [], 16
    while off + 56 <= len(d):
        name = d[off : off + 40].decode()
        stored, fmt, crc, size = struct.unpack_from("<4I", d, off + 40)
        end = off + 56 + stored
        if end > len(d):
            break
        out.append((name, fmt, crc, size, d[off + 56 : end]))
        off = end
    return d[:16], out


def foz_payload(fmt, stored):
    return zlib.decompress(stored) if fmt == 2 else stored


def foz_entry(name, payload):
    crc = zlib.crc32(payload) & 0xFFFFFFFF
    return name.encode() + struct.pack("<4I", len(payload), 1, crc, len(payload)) + payload


def join_module(json_header, code):
    """A shader module payload: its JSON, a NUL, then the code as LEB128 varints, one per 32-bit word."""
    (_, m), = json.loads(json_header)["shaderModules"].items()
    enc = bytearray()
    for w in struct.unpack(f"<{len(code) // 4}I", code):
        while True:
            b = w & 0x7F
            w >>= 7
            if w:
                enc.append(b | 0x80)
            else:
                enc.append(b)
                break
    if len(enc) != m["varintSize"] or m["varintOffset"] != 0:
        raise ValueError("shader module re-encodes to a different size")
    return json_header + b"\0" + bytes(enc)


_vpks = {}


def _resolve(job):
    game, pack, path, sid, refs = job
    k = (game, pack)
    if k not in _vpks:
        _vpks[k] = VPK(os.path.join(game, pack))
    d = _vpks[k].read(path)
    blob = vcs_static_blob(d, vcs_static_table(d)[sid])
    out = []
    for name, off, size, sha in refs:
        code = blob[off : off + size]
        if hashlib.sha256(code).hexdigest() != sha:
            raise ValueError(f"{path} static {sid} @{off}: not the expected shader (another game version?)")
        out.append((name, code))
    return out


def build(recipe, game, out):
    header, entries = foz_read(recipe)
    groups, refs = {}, {}
    for name, fmt, _crc, _size, stored in entries:
        p = foz_payload(fmt, stored)
        if p.startswith(REF_MAGIC):
            r = json.loads(p[len(REF_MAGIC):])
            refs[name] = r
            groups.setdefault((r["pack"], r["file"], r["static"]), []).append((name, r["offset"], r["size"], r["sha256"]))
    for pack in sorted({g[0] for g in groups}):
        if not os.path.exists(os.path.join(game, pack)):
            print(f"pipelines: {os.path.join(game, pack)} is missing", file=sys.stderr)
            return 1
    codes = {}
    jobs = [(game, pack, path, sid, rs) for (pack, path, sid), rs in groups.items()]
    with Pool(min(12, os.cpu_count() or 4)) as pool:
        for got in pool.imap_unordered(_resolve, jobs):
            codes.update(got)
    tmp = out + ".part"
    with open(tmp, "wb") as f:
        f.write(header)
        for name, fmt, crc, size, stored in entries:
            if name in refs:
                f.write(foz_entry(name, join_module(refs[name]["json"].encode(), codes[name])))
            else:
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    os.replace(tmp, out)
    print(f"pipelines: {out} ({len(entries)} entries, {len(refs)} shader modules from {len(groups)} shader combos)")
    return 0


def check(recipe):
    _, entries = foz_read(recipe)
    bad = 0
    for name, fmt, _crc, _size, stored in entries:
        p = foz_payload(fmt, stored)
        if name[22:24] == "04" and not p.startswith(REF_MAGIC):
            bad += 1
        if SPIRV_MAGIC in p:
            bad += 1
    print(f"{recipe}: {len(entries)} entries, {'no shader code' if not bad else f'{bad} entries carry shader code'}")
    return 1 if bad else 0


def info(path):
    _, entries = foz_read(path)
    tags = collections.Counter(name[22:24] for name, *_ in entries)
    samples, refs, code = collections.Counter(), 0, 0
    for name, fmt, _crc, _size, stored in entries:
        p = foz_payload(fmt, stored)
        if name[22:24] == "04":
            refs += p.startswith(REF_MAGIC)
            code += not p.startswith(REF_MAGIC)
        elif name[22:24] == "06":
            for v in (json.loads(p).get("graphicsPipelines") or {}).values():
                samples[(v.get("multisampleState") or {}).get("rasterizationSamples")] += 1
    print(f"{path}: {len(entries)} entries")
    for tag, n in sorted(tags.items()):
        print(f"  {n:6d}  {TAG_NAMES.get(tag, 'tag ' + tag)}")
    print(f"  shader modules: {refs} as references, {code} with code")
    print("  graphics pipelines by samples: " + ", ".join(f"{k}x: {n}" for k, n in sorted(samples.items(), key=lambda kv: str(kv[0]))))
    return 0


if __name__ == "__main__":
    a = sys.argv[1:]
    if len(a) == 4 and a[0] == "build":
        sys.exit(build(a[1], a[2], a[3]))
    if len(a) == 2 and a[0] == "check":
        sys.exit(check(a[1]))
    if len(a) == 2 and a[0] == "info":
        sys.exit(info(a[1]))
    sys.exit(__doc__)
