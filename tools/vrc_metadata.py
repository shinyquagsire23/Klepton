#!/usr/bin/env python3
"""Decrypt VRChat's protected `global-metadata.dat`, and read it.

Why this exists
---------------
VRChat is the only guest in this tree whose IL2CPP metadata is protected, and
that protection is what stands between us and every "what does the managed side
actually do here?" question on the biggest APK in the project — most immediately
the `VRCUiUnderConstruction` wall.

Two doors were tried before this one and both are closed, so do not re-open them:

  * `il2cpp_domain_get` and the rest of the embedding API are absent from
    `libil2cpp.so`'s exports, so `kl_mprobe` cannot attach — the names are
    MANGLED, not missing (2241 dynsyms, 3 matching `il2cpp_*`).
  * `KL_DUMP_METADATA` scanned 6.2 GB of the running process for a plaintext
    image and there is none, anywhere, at any point. That measurement was
    right and its conclusion — "so it must be decrypted per access" — was
    wrong in an interesting way: the file is decrypted ONCE, but into eight
    separate heap buffers, so no contiguous copy of it ever exists.

How the protection works (measured, from MetadataCache::Initialize)
-------------------------------------------------------------------
`libil2cpp.so`'s runtime `.text` is obfuscated — it makes zero direct PLT calls
and its exported entry points are trampolines through a table filled at load —
so none of this was findable statically. It was found by trapping the mapping:
`KL_META_WATCH=1` takes the metadata pages away with `mprotect` and lets the
readers announce themselves as faults, which named the mmap site
(`libil2cpp.so+0x56342b4`), and a conservative stack x-ray from there gave the
chain up to `MetadataLoader::LoadMetadataFile` (0x561b998, identified by its own
"ERROR: Could not open %s") and its single caller, `MetadataCache::Initialize`
at **0x569fdac**. The whole scheme is in that one function:

  * the FILENAME is built on the stack and XORed with `0xf6 - i`;
  * the HEADER is 0x158 bytes at file offset 0, XORed with `i - 0x57`;
  * SEVEN tables are copied out and XORed with a linear keystream that is a
    function of the table's own file offset: either `off + i + 0x1b` or
    `i - off - 0x1b`;
  * everything else in the file is PLAINTEXT and read straight out of the
    mapping. That is why the identifier strings were always greppable and why
    the file looked "selectively encrypted" — it is.

The header is a permutation: its 43 (offset, size) pairs are shuffled, carry no
sanity or version word, and each stored offset is short of the real one by a
per-table constant baked into the code (`hdr[field] + 0x14`, `+ 0x24`, `+ 0x30`,
…). There are more pairs than tables, so some are decoys.

None of that has to be unscrambled, because the file itself says where the
tables are: they are laid out CONTIGUOUSLY in the canonical IL2CPP order, so
seven exact anchors read out of the code plus the tokens the tables carry
(0x02 for typeDefinitions, 0x06 for methods, 0x04 for fields, 0x17 for
properties, 0x14 for events, 0x08 for parameters) pin the rest by tiling. Every
boundary below is confirmed by BOTH — a size that divides exactly by the record
size, and a token sequence that starts at the record we predicted.

The one cross-check worth keeping: `images` is 9120 bytes of 40-byte records and
`assemblies` is 14592 bytes of 64-byte records, i.e. **228 of each**. Those two
counts are equal in every IL2CPP file and were derived here independently.

What is NOT resolved: the tables after `fieldRefs` — attributeData,
attributeDataRange, the unresolved-indirect-call pair, the WinRT pair and
exportedTypeDefinitions. They are the last 2.6 MB, they are all plaintext, and
nothing needs them to read type and method names. `--map` prints them as one
unresolved span rather than guessing.

Usage
-----
    tools/vrc_metadata.py --map                     the table map, with checks
    tools/vrc_metadata.py --decrypt out.dat         the file, XOR undone
    tools/vrc_metadata.py --types 'VRCUi.*'         types matching a regex
    tools/vrc_metadata.py --methods 'VRCUiUnderConstruction'
    tools/vrc_metadata.py --refs 0x1234             which type owns a token
"""
import argparse
import re
import struct
import sys

DEFAULT_SRC = 'vrchat/assets/bin/Data/Managed/Metadata/global-metadata.dat'

HEADER_SIZE = 0x158

# (size_field, offset_field, delta, key) transcribed instruction by instruction
# from MetadataCache::Initialize at 0x569feb0 .. 0x56a0120. `key` is which of the
# two keystreams the loop uses:
#   'plus'  ->  k = (off + i + 0x1b) & 0xff      (add w11, w9, w8; add w11, w11, #0x1b)
#   'minus' ->  k = (i - off - 0x1b) & 0xff      (sub x9, x8, off; add; sub #0x1b)
ENCRYPTED = [
    (0x6c, 0x84, 0x30, 'minus'),   # properties
    (0xc4, 0x80, 0x14, 'plus'),    # assemblies
    (0xe8, 0x10, 0x24, 'plus'),    # stringLiteral
    (0x44, 0x138, 0x28, 'minus'),  # fields
    (0xd0, 0x10c, 0x2c, 'plus'),   # methods
    (0x04, 0x90, 0x30, 'plus'),    # stringLiteralData
    (0x4c, 0x98, 0x14, 'minus'),   # string
]

# The canonical IL2CPP order, with the record size each table is checked
# against. `None` is a byte blob with no record structure. `token` is the
# metadata table byte a record's token field carries, where it has one — the
# second, independent confirmation of every boundary.
LAYOUT = [
    # name                              record  token  token_at
    ('stringLiteral',                        8,  None, None),
    ('stringLiteralData',                 None,  None, None),
    ('string',                            None,  None, None),
    ('events',                              24,  0x14,   20),
    ('properties',                          20,  0x17,   16),
    ('methods',                             32,  0x06,   20),
    ('parameterDefaultValues',              12,  None, None),
    ('fieldDefaultValues',                  12,  None, None),
    ('fieldAndParameterDefaultValueData',None,  None, None),
    ('fieldMarshaledSizes',                 12,  None, None),
    ('parameters',                          12,  0x08,    4),
    ('fields',                              12,  0x04,    8),
    ('genericParameters',                   16,  None, None),
    ('genericParameterConstraints',          4,  None, None),
    ('genericContainers',                   16,  None, None),
    ('nestedTypes',                          4,  None, None),
    ('interfaces',                           4,  None, None),
    ('vtableMethods',                        4,  None, None),
    ('interfaceOffsets',                     8,  None, None),
    ('typeDefinitions',                     88,  0x02,   84),
    ('images',                              40,  None, None),
    ('assemblies',                          64,  None, None),
    ('fieldRefs',                            8,  None, None),
]

# Table extents, in canonical order. Every one is either an anchor read out of
# the code (marked) or the tile that contiguity leaves between two anchors; all
# of them are then checked against the record size and, where there is one, the
# token sequence. See --map, which re-derives the checks rather than trusting
# this list.
EXTENTS = [
    (0x00000158,   410792),   # stringLiteral      <- code anchor
    (0x00064600,  1862468),   # stringLiteralData  <- code anchor
    (0x0022b144,  7521668),   # string             <- code anchor
    (0x009576c8,    18744),   # events
    (0x0095c000,   796100),   # properties         <- code anchor
    (0x00a1e5c4,  9675168),   # methods            <- code anchor
    (0x01358764,   108672),   # parameterDefaultValues
    (0x01372fe4,   370356),   # fieldDefaultValues
    (0x013cd698,  1177376),   # fieldAndParameterDefaultValueData
    (0x014ecdb8,   128484),   # fieldMarshaledSizes
    (0x0150c39c,  3754896),   # parameters
    (0x018a0f2c,  1743168),   # fields             <- code anchor
    (0x01a4a86c,   195904),   # genericParameters
    (0x01a7a5ac,    10996),   # genericParameterConstraints
    (0x01a7d0a0,   111888),   # genericContainers
    (0x01a985b0,    50008),   # nestedTypes
    (0x01aa4908,    50672),   # interfaces
    (0x01ab0ef8,  1435288),   # vtableMethods
    (0x01c0f590,   245072),   # interfaceOffsets
    (0x01c4b2e0,  2830784),   # typeDefinitions
    (0x01efe4a0,     9120),   # images
    (0x01f00840,    14592),   # assemblies         <- code anchor
    (0x01f04140,     8904),   # fieldRefs
]


def decrypt(path):
    """The file with every XORed region undone. Everything else is untouched."""
    d = bytearray(open(path, 'rb').read())
    h = bytearray(d[:HEADER_SIZE])
    for i in range(HEADER_SIZE):
        h[i] ^= (i - 0x57) & 0xff
    words = list(struct.unpack('<%dI' % (HEADER_SIZE // 4), bytes(h)))
    d[:HEADER_SIZE] = h
    regions = []
    for size_field, off_field, delta, key in ENCRYPTED:
        off = words[off_field // 4] + delta
        size = words[size_field // 4]
        for i in range(size):
            k = (off + i + 0x1b) if key == 'plus' else (i - off - 0x1b)
            d[off + i] ^= k & 0xff
        regions.append((off, size, key))
    return d, words, regions


def encrypt(d, regions):
    """The inverse of `decrypt`, which is the same transform again — every
    region is a XOR against a keystream that does not depend on the data, so
    applying it twice is the identity. This is what makes a PATCHED file
    possible: edit the plaintext, run this, and the guest's own loader accepts
    it without knowing anything happened."""
    d = bytearray(d)
    for off, size, key in regions:
        for i in range(size):
            k = (off + i + 0x1b) if key == 'plus' else (i - off - 0x1b)
            d[off + i] ^= k & 0xff
    for i in range(HEADER_SIZE):
        d[i] ^= (i - 0x57) & 0xff
    return bytes(d)


class Metadata:
    def __init__(self, path):
        self.data, self.words, self.regions = decrypt(path)
        self.t = {}
        for (name, _rec, _tok, _at), (off, size) in zip(LAYOUT, EXTENTS):
            self.t[name] = (off, size)

    def literal(self, i):
        off = self.t['stringLiteral'][0] + i * 8
        ln, di = struct.unpack_from('<ii', self.data, off)
        return ln, di, self.data[self.t['stringLiteralData'][0] + di:
                                 self.t['stringLiteralData'][0] + di + ln]

    def literal_count(self):
        return self.t['stringLiteral'][1] // 8

    def alias_literal(self, dst, src):
        """Point literal `dst` at literal `src`'s DATA — an 8-byte edit of the
        stringLiteral table and nothing else. Every managed `"..."` constant
        that resolves through `dst` then yields `src`'s text, which is how a
        navigation target is redirected without moving a byte of code."""
        off = self.t['stringLiteral'][0] + dst * 8
        struct.pack_into('<ii', self.data, off,
                         *struct.unpack_from('<ii', self.data,
                                             self.t['stringLiteral'][0] + src * 8))

    def string(self, index):
        off = self.t['string'][0] + index
        end = self.data.index(b'\0', off)
        return self.data[off:end].decode('utf-8', 'replace')

    def records(self, table, size):
        off, total = self.t[table]
        for i in range(total // size):
            yield i, off + i * size

    def type_def(self, i):
        off = self.t['typeDefinitions'][0] + i * 88
        f = struct.unpack_from('<8i', self.data, off)          # name..flags
        g = struct.unpack_from('<8i', self.data, off + 32)     # starts
        h = struct.unpack_from('<8H', self.data, off + 64)     # counts
        token = struct.unpack_from('<I', self.data, off + 84)[0]
        return dict(nameIndex=f[0], namespaceIndex=f[1], byvalTypeIndex=f[2],
                    declaringTypeIndex=f[3], parentIndex=f[4], elementTypeIndex=f[5],
                    genericContainerIndex=f[6], flags=f[7] & 0xffffffff,
                    fieldStart=g[0], methodStart=g[1], eventStart=g[2],
                    propertyStart=g[3], nestedTypesStart=g[4], interfacesStart=g[5],
                    vtableStart=g[6], interfaceOffsetsStart=g[7],
                    method_count=h[0], property_count=h[1], field_count=h[2],
                    event_count=h[3], nested_type_count=h[4], vtable_count=h[5],
                    interfaces_count=h[6], interface_offsets_count=h[7],
                    token=token)

    def type_name(self, i):
        td = self.type_def(i)
        ns = self.string(td['namespaceIndex'])
        nm = self.string(td['nameIndex'])
        return (ns + '.' + nm) if ns else nm

    def method(self, i):
        off = self.t['methods'][0] + i * 32
        name, decl, ret, pstart, gen, token = struct.unpack_from('<6i', self.data, off)
        flags, iflags, slot, pcount = struct.unpack_from('<4H', self.data, off + 24)
        return dict(nameIndex=name, declaringType=decl, returnType=ret,
                    parameterStart=pstart, genericContainerIndex=gen,
                    token=token & 0xffffffff, flags=flags, iflags=iflags,
                    slot=slot, parameterCount=pcount)

    def type_count(self):
        return self.t['typeDefinitions'][1] // 88

    def method_count(self):
        return self.t['methods'][1] // 32

    def image(self, i):
        off = self.t['images'][0] + i * 40
        v = struct.unpack_from('<10i', self.data, off)
        return dict(nameIndex=v[0], assemblyIndex=v[1], typeStart=v[2],
                    typeCount=v[3] & 0xffffffff, entryPointIndex=v[6],
                    token=v[7] & 0xffffffff)


def cmd_map(md, args):
    n = len(md.data)
    print("file %d bytes, header 0x%x, %d header words (43 pairs, shuffled, "
          "some decoys)" % (n, HEADER_SIZE, len(md.words)))
    print("\nXORed regions, from MetadataCache::Initialize (0x569fdac):")
    for off, size, key in sorted(md.regions):
        print("  0x%08x .. 0x%08x  %10d  key=%s" % (off, off + size, size, key))
    print("\ntable map, canonical order — every row checked, not asserted:")
    prev = HEADER_SIZE
    ok = True
    for (name, rec, tok, at), (off, size) in zip(LAYOUT, EXTENTS):
        notes = []
        if off != prev:
            notes.append("GAP/OVERLAP of %d before it" % (off - prev)); ok = False
        if rec:
            if size % rec:
                notes.append("size %% %d = %d" % (rec, size % rec)); ok = False
            else:
                notes.append("%d records of %d" % (size // rec, rec))
        if tok is not None:
            # The TABLE byte of the token, across every record we can afford to
            # look at — not "the first record's token is RID 1", which is true
            # of the tables written in RID order and false of `parameters`,
            # whose records are ordered by the method that owns them. Checking
            # the top byte is the property that actually identifies the table,
            # and checking it at the far end is what confirms the SIZE.
            bad = 0
            total = size // rec
            for k in range(0, total, max(1, total // 4096)):
                if struct.unpack_from('<I', md.data, off + k * rec + at)[0] >> 24 != tok:
                    bad += 1
            notes.append("token byte 0x%02x on %s" %
                         (tok, "every record sampled" if not bad else
                          "%d of the records sampled — WRONG" % bad))
            if bad:
                ok = False
        print("  %-34s 0x%08x .. 0x%08x  %10d   %s"
              % (name, off, off + size, size, "; ".join(notes)))
        prev = off + size
    print("  %-34s 0x%08x .. 0x%08x  %10d   attributeData, attributeDataRange, "
          "the unresolved-indirect-call pair, the WinRT pair and "
          "exportedTypeDefinitions — all PLAINTEXT, none needed to read names"
          % ("<unresolved tail>", prev, n, n - prev))
    print("\nimages %d / assemblies %d  (these must be equal, and they were "
          "derived independently)" % (md.t['images'][1] // 40, md.t['assemblies'][1] // 64))
    print("types %d, methods %d" % (md.type_count(), md.method_count()))
    print("\nmap %s" % ("CONSISTENT" if ok else "INCONSISTENT — do not trust a dump"))
    return 0 if ok else 1


def cmd_decrypt(md, args):
    open(args.decrypt, 'wb').write(bytes(md.data))
    print("wrote %s (%d bytes). The XORed regions are undone and the header is "
          "plaintext; it is NOT a stock-format file — the header carries no "
          "sanity or version word and its pairs are shuffled, so Il2CppDumper "
          "will refuse it. Use --types/--methods, or tools/vrc_metadata.py as a "
          "library." % (args.decrypt, len(md.data)))
    return 0


def cmd_types(md, args):
    rx = re.compile(args.types, re.I)
    n = 0
    for i in range(md.type_count()):
        name = md.type_name(i)
        if rx.search(name):
            td = md.type_def(i)
            print("[%6d] %-70s token 0x%08x  methods %d @%d  fields %d"
                  % (i, name, td['token'], td['method_count'], td['methodStart'],
                     td['field_count']))
            n += 1
    print("%d type(s)" % n)
    return 0


def cmd_methods(md, args):
    rx = re.compile(args.methods, re.I)
    n = 0
    for i in range(md.type_count()):
        name = md.type_name(i)
        if not rx.search(name):
            continue
        td = md.type_def(i)
        print("%s   (type %d, token 0x%08x)" % (name, i, td['token']))
        for k in range(td['method_count']):
            m = md.method(td['methodStart'] + k)
            print("    %-60s token 0x%08x  rid %-6d flags 0x%04x  params %d"
                  % (md.string(m['nameIndex']), m['token'], m['token'] & 0xffffff,
                     m['flags'], m['parameterCount']))
            n += 1
    print("%d method(s)" % n)
    return 0


def cmd_literals(md, args):
    rx = re.compile(args.literals, re.I)
    n = 0
    for i in range(md.literal_count()):
        ln, di, raw = md.literal(i)
        if ln < 0 or ln > 8192 or di < 0:
            continue
        s = raw.decode('utf-8', 'replace')
        if rx.search(s):
            print("[%6d] %s" % (i, s))
            n += 1
    print("%d literal(s)" % n)
    return 0


def cmd_patch(md, args):
    """Write a patched, RE-ENCRYPTED metadata file the guest loads as its own."""
    for spec in args.alias_literal:
        dst, src = (int(x, 0) for x in spec.split('=', 1))
        before = md.literal(dst)[2].decode('utf-8', 'replace')
        md.alias_literal(dst, src)
        after = md.literal(dst)[2].decode('utf-8', 'replace')
        print("literal %d: %r -> %r  (aliased to literal %d)"
              % (dst, before, after, src))
    open(args.patch, 'wb').write(encrypt(md.data, md.regions))
    print("wrote %s (%d bytes), re-encrypted — drop it in as "
          "global-metadata.dat" % (args.patch, len(md.data)))
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--src', default=DEFAULT_SRC)
    ap.add_argument('--map', action='store_true')
    ap.add_argument('--decrypt', metavar='OUT')
    ap.add_argument('--types', metavar='REGEX')
    ap.add_argument('--methods', metavar='REGEX')
    ap.add_argument('--literals', metavar='REGEX',
                    help='search the managed string LITERALS')
    ap.add_argument('--patch', metavar='OUT',
                    help='write a patched, re-encrypted metadata file')
    ap.add_argument('--alias-literal', metavar='DST=SRC', action='append',
                    default=[], help='point literal DST at literal SRC\'s text')
    args = ap.parse_args()
    md = Metadata(args.src)
    if args.map:
        return cmd_map(md, args)
    if args.literals:
        return cmd_literals(md, args)
    if args.patch:
        return cmd_patch(md, args)
    if args.decrypt:
        return cmd_decrypt(md, args)
    if args.types:
        return cmd_types(md, args)
    if args.methods:
        return cmd_methods(md, args)
    ap.print_help()
    return 0


if __name__ == '__main__':
    sys.exit(main())
