#!/usr/bin/env python3
"""A Unity game's Vulkan pipeline list, rebuilt from the player's own copy of the game.

A pipeline list is a Fossilize archive (.foz): shader modules plus the Vulkan state that makes pipelines of them.
Compiling it before play keeps Metal's 150-600 ms first-use shader compiles out of the game. The shader modules are
the game's own SPIR-V and are not ours to pass on — but Unity ships each of them in the game's files: SMOL-V
encoded, inside an LZ4 block of a Shader object, inside a (UnityFS) asset bundle or a serialized file, inside the
APK, the OBB or a loose bundle. So what is passed on is a RECIPE: the archive with every shader module replaced by
where to find it (file, archive member, bundle node, object, block, offset, size, SHA-256). This script makes a
recipe from a recorded archive and a copy of the game, and turns a recipe back into the archive on another copy.

    unity_pipelines.py merge  <out.foz> <a.foz> <b.foz> ...               several recordings in one, each entry once
    unity_pipelines.py recipe <recorded.foz> <game dir> <out.pipelines>   replace the shader code by where it is
    unity_pipelines.py build  <recipe> <game dir> <out.foz>               rebuild the archive from the game's files
    unity_pipelines.py check  <recipe>                                    a recipe must carry no shader code
    unity_pipelines.py info   <recipe or .foz>                            what is in it

<game dir> is the folder the game's files lie in, flat: the APK, the OBB(s) and the loose .bundle files (AVP Play's
downloads folder for the game).

The same idea as tools/alyx_pipelines.py (whose archive helpers this uses), for Unity instead of Source 2. SMOL-V is
decoded with Aras Pranckevičius' own decoder (vendor-smolv/, public domain or MIT), compiled on first use.
"""
import collections
import ctypes
import hashlib
import json
import lzma
import os
import struct
import subprocess
import sys
import zipfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from alyx_pipelines import foz_read, foz_payload, foz_entry, join_module, TAG_NAMES  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REF_MAGIC = b"KLUREF"
SPIRV_MAGIC = b"\x03\x02\x23\x07"
SMOLV_MAGIC = b"LOMS"
SHADER = 48                     # Unity's class id of a Shader

# ---- decompression -------------------------------------------------------------------------------------------------

_lc = ctypes.CDLL("/usr/lib/libcompression.dylib")
_lc.compression_decode_buffer.restype = ctypes.c_size_t
_lc.compression_decode_buffer.argtypes = [ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                                          ctypes.c_void_p, ctypes.c_int]


def lz4(data, usize):
    out = ctypes.create_string_buffer(usize)
    if _lc.compression_decode_buffer(out, usize, data, len(data), None, 0x101) != usize:    # COMPRESSION_LZ4_RAW
        raise ValueError("an LZ4 block did not unpack to its stated size")
    return out.raw


def _unlzma(data, usize):
    props, dict_size = data[0], struct.unpack_from("<I", data, 1)[0]
    f = {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": props % 9, "lp": (props // 9) % 5, "pb": props // 45}
    return lzma.LZMADecompressor(lzma.FORMAT_RAW, filters=[f]).decompress(data[5:], usize)


def _unpack(kind, data, usize):
    if kind == 0:
        return data
    if kind == 1:
        return _unlzma(data, usize)
    if kind in (2, 3):
        return lz4(data, usize)
    raise ValueError(f"compression {kind} is not known here")


_smolv = None


def smolv():
    """The decoder, built from vendor-smolv/ when it is not there yet."""
    global _smolv
    if _smolv is None:
        src = os.path.join(ROOT, "vendor-smolv")
        lib = os.path.join(ROOT, "build", "libklsmolv.dylib")
        parts = [os.path.join(src, n) for n in ("smolv.cpp", "kl_smolv.cpp", "smolv.h")]
        if not os.path.exists(lib) or any(os.path.getmtime(p) > os.path.getmtime(lib) for p in parts):
            os.makedirs(os.path.dirname(lib), exist_ok=True)
            subprocess.run(["xcrun", "clang++", "-std=c++17", "-O2", "-shared", "-o", lib, parts[0], parts[1]], check=True)
        _smolv = ctypes.CDLL(lib)
        _smolv.kl_smolv_decoded_size.restype = ctypes.c_size_t
        _smolv.kl_smolv_decoded_size.argtypes = [ctypes.c_char_p, ctypes.c_size_t]
        _smolv.kl_smolv_decode.restype = ctypes.c_long
        _smolv.kl_smolv_decode.argtypes = [ctypes.c_char_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t]
    return _smolv


def smolv_decode(block, at):
    """The SPIR-V of the SMOL-V program that starts at block[at], or None."""
    n = smolv().kl_smolv_decoded_size(block[at : at + 24], 24)
    if not 20 <= n <= (16 << 20) or n % 4:
        return None
    out = ctypes.create_string_buffer(n)
    if smolv().kl_smolv_decode(block[at:], len(block) - at, out, n) <= 0:
        return None
    return out.raw


# ---- Unity's containers --------------------------------------------------------------------------------------------

def _cstr(d, o):
    e = d.index(b"\0", o)
    return d[o:e].decode("latin1"), e + 1


def unityfs_nodes(d):
    """[(node name, bytes)] of a UnityFS bundle: its data blocks unpacked and cut at the directory."""
    sig, o = _cstr(d, 0)
    if sig != "UnityFS":
        raise ValueError("not a UnityFS bundle")
    ver = struct.unpack_from(">I", d, o)[0]
    o += 4
    _, o = _cstr(d, o)
    _, o = _cstr(d, o)
    _size, csize, usize, flags = struct.unpack_from(">QIII", d, o)
    o += 20
    if ver >= 7:
        o = (o + 15) & ~15
    if flags & 0x80:
        info = d[len(d) - csize :]
    else:
        info = d[o : o + csize]
        o += csize
    info = _unpack(flags & 0x3F, info, usize)
    if flags & 0x200:
        o = (o + 15) & ~15
    p = 16
    (nblocks,) = struct.unpack_from(">I", info, p)
    p += 4
    data = bytearray()
    for _ in range(nblocks):
        u, c, f = struct.unpack_from(">IIH", info, p)
        p += 10
        data += _unpack(f & 0x3F, d[o : o + c], u)
        o += c
    (nnodes,) = struct.unpack_from(">I", info, p)
    p += 4
    out = []
    for _ in range(nnodes):
        off, sz, _fl = struct.unpack_from(">QQI", info, p)
        p += 20
        name, p = _cstr(info, p)
        out.append((name, bytes(data[off : off + sz])))
    return out


def serialized_objects(d):
    """[(path id, start, size, class id)] of a serialized file, or None where `d` is not one."""
    try:
        _msize, _fsize, ver, doff = struct.unpack_from(">IIII", d, 0)
        if not 17 <= ver <= 60:
            return None
        o = 16
        endian = d[o]
        o += 4
        if ver >= 22:
            _msize, fsize, doff, _ = struct.unpack_from(">IQQQ", d, o)
            o += 28
            if fsize != len(d):
                return None
        E = ">" if endian else "<"
        _, o = _cstr(d, o)
        o += 4                                  # target platform
        typetree = d[o]
        o += 1
        (ntypes,) = struct.unpack_from(E + "i", d, o)
        o += 4
        if not 0 <= ntypes < 100000:
            return None
        types = []
        for _ in range(ntypes):
            (cid,) = struct.unpack_from(E + "i", d, o)
            o += 4 + 1                          # class id, isStripped
            o += 2                              # script type index
            if cid == 114:
                o += 16                         # script id
            o += 16                             # old type hash
            if typetree:
                nn, sb = struct.unpack_from(E + "ii", d, o)
                o += 8 + nn * 32 + sb
                if ver >= 21:
                    (nd,) = struct.unpack_from(E + "i", d, o)
                    o += 4 + nd * 4
            types.append(cid)
        (nobj,) = struct.unpack_from(E + "i", d, o)
        o += 4
        if not 0 <= nobj < 10000000:
            return None
        objs = []
        for _ in range(nobj):
            o = (o + 3) & ~3
            if ver >= 22:
                pid, start, size, tid = struct.unpack_from(E + "qQIi", d, o)
                o += 24
            else:
                pid, start, size, tid = struct.unpack_from(E + "qIIi", d, o)
                o += 20
            if not 0 <= tid < len(types) or doff + start + size > len(d):
                return None
            objs.append((pid, doff + start, size, types[tid]))
        return objs
    except (struct.error, ValueError, IndexError):
        return None


def shader_blocks(obj):
    """The compressed shader blobs of a Shader object: [(position of the blob, offsets, packed sizes, sizes)].

    Found by shape rather than by walking the fields in front of it (whose layout moves with every Unity version):
    platforms = [one id], offsets = [[...]], compressedLengths = [[...]], decompressedLengths = [[...]], then a byte
    array as long as the packed sizes add up to. A wrong hit yields a block that does not unpack, or programs whose
    checksum is not the one looked for.
    """
    out = []
    n = len(obj)
    for i in range(0, n - 40, 4):
        a, plat, b = struct.unpack_from("<III", obj, i)
        if a != 1 or plat > 64 or b != 1:
            continue
        p = i + 8
        try:
            (nch,) = struct.unpack_from("<I", obj, p + 4)
            if not 0 < nch < 4096:
                continue
            offs = struct.unpack_from(f"<{nch}I", obj, p + 8)
            p += 8 + 4 * nch
            if struct.unpack_from("<II", obj, p) != (1, nch):
                continue
            cl = struct.unpack_from(f"<{nch}I", obj, p + 8)
            p += 8 + 4 * nch
            if struct.unpack_from("<II", obj, p) != (1, nch):
                continue
            dl = struct.unpack_from(f"<{nch}I", obj, p + 8)
            p += 8 + 4 * nch
            (total,) = struct.unpack_from("<I", obj, p)
            p += 4
            if total != sum(cl) or p + total > n:
                continue
            out.append((p, offs, cl, dl))
        except struct.error:
            continue
    return out


def block_of(obj, blob, k):
    p, offs, cl, dl = blob
    return lz4(obj[p + offs[k] : p + offs[k] + cl[k]], dl[k])


# ---- a game's files ------------------------------------------------------------------------------------------------

def containers(game):
    """Every place in a game's folder that can hold serialized files: (file, member, bytes), lazily.

    Loose .bundle files, and the members of the APK and the OBBs — with a file Unity cut into .splitN pieces put
    together again.
    """
    for name in sorted(os.listdir(game)):
        path = os.path.join(game, name)
        if not os.path.isfile(path):
            continue
        low = name.lower()
        if low.endswith(".bundle"):
            yield name, "", lambda path=path: open(path, "rb").read()
        elif low.endswith((".apk", ".obb")):
            try:
                z = zipfile.ZipFile(path)
            except zipfile.BadZipFile:
                continue
            splits = collections.defaultdict(list)
            for m in z.namelist():
                base, _, tail = m.rpartition(".split")
                if base and tail.isdigit():
                    splits[base].append((int(tail), m))
                elif not m.endswith("/") and (m.lower().endswith(".bundle") or "/bin/Data/" in m):
                    yield name, m, lambda z=z, m=m: z.read(m)
            for base, parts in splits.items():
                yield name, base, lambda z=z, parts=parts: b"".join(z.read(m) for _, m in sorted(parts))


def read_container(game, file, member):
    path = os.path.join(game, file)
    if not member:
        return open(path, "rb").read()
    z = zipfile.ZipFile(path)
    if member in z.namelist():
        return z.read(member)
    parts = sorted((int(m.rpartition(".split")[2]), m) for m in z.namelist()
                   if m.startswith(member + ".split") and m.rpartition(".split")[2].isdigit())
    if not parts:
        raise KeyError(f"{file} has no {member}")
    return b"".join(z.read(m) for _, m in parts)


def serialized_files(data):
    """[(node name, bytes)] of the serialized files in a container's bytes: a bundle's nodes, or the file itself."""
    if data[:8] == b"UnityFS\0":
        return [(n, d) for n, d in unityfs_nodes(data) if not n.endswith((".resS", ".resource"))]
    return [("", data)]


def programs(game, report=None):
    """Every SMOL-V program in the game's shaders: (location, spirv)."""
    for file, member, load in containers(game):
        try:
            data = load()
            files = serialized_files(data)
        except (ValueError, struct.error, lzma.LZMAError, zipfile.BadZipFile, OSError):
            continue
        count = 0
        for node, d in files:
            objs = serialized_objects(d)
            if not objs:
                continue
            for pid, start, size, cid in objs:
                if cid != SHADER:
                    continue
                obj = d[start : start + size]
                for bi, blob in enumerate(shader_blocks(obj)):
                    for k in range(len(blob[1])):
                        try:
                            block = block_of(obj, blob, k)
                        except ValueError:
                            continue
                        at = block.find(SMOLV_MAGIC)
                        while at >= 0:
                            code = smolv_decode(block, at)
                            if code:
                                count += 1
                                yield {"file": file, "member": member, "node": node, "path": pid, "blob": bi,
                                       "block": k, "offset": at, "size": len(code)}, code
                            at = block.find(SMOLV_MAGIC, at + 4)
        if report and count:
            report(file, member, count)


# ---- recipe <-> archive --------------------------------------------------------------------------------------------

def module_code(payload):
    """(json header, spirv) of a Fossilize shader-module payload."""
    z = payload.index(b"\0")
    words, w, sh = [], 0, 0
    for b in payload[z + 1 :]:
        w |= (b & 0x7F) << sh
        if b & 0x80:
            sh += 7
        else:
            words.append(w)
            w = sh = 0
    return payload[:z], struct.pack(f"<{len(words)}I", *words)


def recipe(recorded, game, out):
    head, entries = foz_read(recorded)
    wanted = {}
    for name, fmt, _crc, _size, stored in entries:
        if name[:24].endswith("04"):
            hdr, code = module_code(foz_payload(fmt, stored))
            wanted[hashlib.sha256(code).hexdigest()] = (name, hdr)
    found = {}
    seen = [0]

    def report(file, member, count):
        seen[0] += count
        print(f"  {file}{' : ' + member if member else ''}: {count} programs", flush=True)

    for where, code in programs(game, report):
        h = hashlib.sha256(code).hexdigest()
        if h in wanted and h not in found:
            found[h] = where
    by_name = {wanted[h][0]: (h, wanted[h][1], w) for h, w in found.items()}
    kept = dropped = 0
    with open(out, "wb") as f:
        f.write(head)
        for name, fmt, crc, size, stored in entries:
            if name[:24].endswith("04"):
                if name not in by_name:
                    dropped += 1
                    continue
                h, hdr, where = by_name[name]
                ref = dict(where, sha256=h, header=hdr.decode())
                f.write(foz_entry(name, REF_MAGIC + json.dumps(ref, sort_keys=True).encode()))
                kept += 1
            else:
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    print(f"{out}: {kept} shader modules replaced by where they are, {dropped} not found in this copy of the game "
          f"(of {seen[0]} programs in it)")
    return 0 if kept else 1


def build(recipe_path, game, out):
    head, entries = foz_read(recipe_path)
    refs = collections.defaultdict(list)
    for name, fmt, _crc, _size, stored in entries:
        if name[:24].endswith("04"):
            p = foz_payload(fmt, stored)
            if not p.startswith(REF_MAGIC):
                raise SystemExit(f"{recipe_path}: a shader module that is not a reference — this is not a recipe")
            r = json.loads(p[len(REF_MAGIC) :])
            refs[(r["file"], r["member"])].append((name, r))
    made, missing = {}, 0
    for (file, member), want in sorted(refs.items()):
        try:
            files = dict(serialized_files(read_container(game, file, member)))
        except (OSError, KeyError, ValueError, struct.error, zipfile.BadZipFile, lzma.LZMAError):
            missing += len(want)
            continue
        objects, blocks = {}, {}
        for name, r in want:
            d = files.get(r["node"])
            if d is None:
                missing += 1
                continue
            if r["node"] not in objects:
                objects[r["node"]] = {pid: (start, size) for pid, start, size, cid in (serialized_objects(d) or [])
                                      if cid == SHADER}
            at = objects[r["node"]].get(r["path"])
            code = None
            if at:
                key = (r["node"], r["path"], r["blob"], r["block"])
                if key not in blocks:
                    obj = d[at[0] : at[0] + at[1]]
                    blobs = shader_blocks(obj)
                    try:
                        blocks[key] = block_of(obj, blobs[r["blob"]], r["block"]) if r["blob"] < len(blobs) else None
                    except (ValueError, IndexError):
                        blocks[key] = None
                if blocks[key] is not None:
                    code = smolv_decode(blocks[key], r["offset"])
            if not code or hashlib.sha256(code).hexdigest() != r["sha256"]:
                missing += 1                    # another build of the game: this module is not where the recipe says
                continue
            made[name] = join_module(r["header"].encode(), code)
    # What was not where the recipe says: this copy is another build of the game, in which a file was packed anew or
    # a shader moved to another file. The shader itself is usually still there, so the whole copy is searched for the
    # checksums that are still open — once, and only then.
    moved = 0
    open_refs = {r["sha256"]: (name, r) for want in refs.values() for name, r in want if name not in made}
    if open_refs:
        for _where, code in programs(game):
            h = hashlib.sha256(code).hexdigest()
            if h in open_refs:
                name, r = open_refs.pop(h)
                made[name] = join_module(r["header"].encode(), code)
                moved += 1
                if not open_refs:
                    break
    with open(out, "wb") as f:
        f.write(head)
        for name, fmt, crc, size, stored in entries:
            if name[:24].endswith("04"):
                if name in made:
                    f.write(foz_entry(name, made[name]))
            else:
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    total = sum(len(v) for v in refs.values())
    gone = total - len(made)
    print(f"{out}: {len(made)} of {total} shader modules taken from this copy of the game"
          + (f" ({moved} of them from another place than in the build the list was made from)" if moved else "")
          + (f", {gone} are not in it (pipelines that need them are skipped when the list is compiled)" if gone else ""))
    return 0 if made else 1


def merge(out, sources):
    """Recordings of several runs (a walk through every level on a Mac, a played session on a headset) as one."""
    head, seen, total = None, set(), 0
    with open(out, "wb") as f:
        for src in sources:
            h, entries = foz_read(src)
            if head is None:
                head = h
                f.write(head)
            for name, fmt, crc, size, stored in entries:
                total += 1
                if name in seen:
                    continue
                seen.add(name)
                f.write(name.encode() + struct.pack("<4I", len(stored), fmt, crc, size) + stored)
    print(f"{out}: {len(seen)} entries from {len(sources)} recordings ({total - len(seen)} were in more than one)")
    return 0


def check(path):
    _, entries = foz_read(path)
    bad = 0
    for name, fmt, _crc, _size, stored in entries:
        p = foz_payload(fmt, stored)
        if name[:24].endswith("04") and not p.startswith(REF_MAGIC):
            bad += 1
        elif SPIRV_MAGIC in p or SMOLV_MAGIC in p[:64]:
            bad += 1
    print(f"{path}: {'carries shader code in ' + str(bad) + ' entries' if bad else 'no shader code'}")
    return 1 if bad else 0


def info(path):
    _, entries = foz_read(path)
    count, size, refs = collections.Counter(), collections.Counter(), 0
    files = collections.Counter()
    for name, fmt, _crc, _size, stored in entries:
        tag = name[22:24]
        count[tag] += 1
        size[tag] += len(stored)
        if tag == "04":
            p = foz_payload(fmt, stored)
            if p.startswith(REF_MAGIC):
                refs += 1
                files[json.loads(p[len(REF_MAGIC) :])["file"]] += 1
    for tag in sorted(count):
        print(f"  {TAG_NAMES.get(tag, tag):20} {count[tag]:6}  {size[tag]:10} bytes")
    if refs:
        print(f"  a recipe: {refs} shader modules are references, into {len(files)} of the game's files")
    return 0


def main(argv):
    if len(argv) >= 4 and argv[1] == "merge":
        return merge(argv[2], argv[3:])
    if len(argv) == 5 and argv[1] == "recipe":
        return recipe(argv[2], argv[3], argv[4])
    if len(argv) == 5 and argv[1] == "build":
        return build(argv[2], argv[3], argv[4])
    if len(argv) == 3 and argv[1] == "check":
        return check(argv[2])
    if len(argv) == 3 and argv[1] == "info":
        return info(argv[2])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
