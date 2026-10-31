#!/usr/bin/env python3
"""Prepare explicitly selected ARM64 hooks in a klepton-ld image BEFORE signing.

This emits signed entry gates/original trampolines plus zero-initialized data
slots. The runtime only fills/swaps those slots. No executable allocation.
Only thin, unsigned klepton-ld Mach-O files and supported instructions qualify.
"""
import argparse
import hashlib
import json
import struct
from pathlib import Path

PAGE = 16384
MAGIC = b'SC2AOT1\0'
HEADER = struct.Struct('<8sIIIIQ32s')
ENTRY = struct.Struct('<QQQQII')
SEGMENT = struct.Struct('<II16sQQQQiiII')
SECTION = struct.Struct('<16s16sQQIIIIIIII')


def align(n, step=PAGE):
    return (n + step - 1) & -step


def signed(value, bits):
    return (value & ((1 << (bits - 1)) - 1)) - (value & (1 << (bits - 1)))


def branch(source, target, link=False):
    delta = target - source
    if delta % 4 or not -(1 << 27) <= delta < (1 << 27):
        raise ValueError('branch exceeds signed 128 MiB reach')
    return (0x94000000 if link else 0x14000000) | ((delta // 4) & 0x3ffffff)


def adrp(source, target, register):
    delta = (target // 4096) - (source // 4096)
    if not -(1 << 20) <= delta < (1 << 20):
        raise ValueError('ADRP exceeds signed 4 GiB reach')
    imm = delta & 0x1fffff
    return 0x90000000 | ((imm & 3) << 29) | ((imm >> 2) << 5) | register


def add_address(register, target):
    return 0x91000000 | ((target & 4095) << 10) | (register << 5) | register


def relocate(word, source, destination):
    """Relocate one entry instruction; reject classes not covered here."""
    if word & 0x7c000000 == 0x14000000:  # B / BL
        if word & 0x80000000:
            raise ValueError('BL entry requires an unwind-aware trampoline, not yet supported')
        target = source + signed(word & 0x3ffffff, 26) * 4
        return [branch(destination, target, bool(word & 0x80000000))]
    if word & 0x1f000000 == 0x10000000:  # ADR / ADRP
        rd = word & 31
        if rd == 18:
            raise ValueError('untranslated x18 destination')
        imm = signed(((word >> 5) & 0x7ffff) * 4 + ((word >> 29) & 3), 21)
        if word & 0x80000000:
            return [adrp(destination, (source & -4096) + imm * 4096, rd)]
        target = source + imm
        return [adrp(destination, target, rd), add_address(rd, target)]
    if word & 0xff000010 == 0x54000000:  # B.cond
        if word & 15 >= 14:
            raise ValueError('unsupported AL/NV conditional branch')
        target = source + signed((word >> 5) & 0x7ffff, 19) * 4
        inverse = ((word ^ 1) & ~0xffffe0) | (2 << 5)
        return [inverse, branch(destination + 4, target)]
    if word & 0x7e000000 in (0x34000000, 0x36000000):  # CBZ/NZ, TBZ/NZ
        if word & 31 == 18:
            raise ValueError('untranslated x18 conditional branch')
        bits = 14 if word & 0x7e000000 == 0x36000000 else 19
        mask = ((1 << bits) - 1) << 5
        target = source + signed((word & mask) >> 5, bits) * 4
        inverse = ((word ^ 0x01000000) & ~mask) | (2 << 5)
        return [inverse, branch(destination + 4, target)]
    if word & 0x3b000000 == 0x18000000:  # PC-relative literal load
        rt = word & 31
        opc = word >> 30
        vector = bool(word & (1 << 26))
        if not vector and rt == 18:
            raise ValueError('untranslated x18 literal load')
        loads = ([0xbd400000, 0xfd400000, 0x3dc00000] if vector else
                 [0xb9400000, 0xf9400000, 0xb9800000])
        if opc >= len(loads):
            raise ValueError('unsupported prefetch/reserved literal instruction')
        target = source + signed((word >> 5) & 0x7ffff, 19) * 4
        return [adrp(destination, target, 17), add_address(17, target),
                loads[opc] | (17 << 5) | rt]
    if word in (0xd503201f, 0xd65f03c0):  # NOP, RET x30
        return [word]
    # Integer unsigned-immediate LDR/STR: bits 10..21 are an offset, not
    # registers. In particular LDR x8,[x0,#0x90] encodes 18 in that field.
    if word & 0xffc00000 in (0xf9400000, 0xf9000000, 0xb9400000, 0xb9000000):
        if 18 in (word & 31, (word >> 5) & 31):
            raise ValueError('untranslated x18 load/store operand')
        return [word]
    # These architectural groups contain no PC-relative operands. ADR and
    # literal loads were handled above. Conservatively reject apparent x18
    # fields, including false positives in immediate/vector encodings.
    group = (word >> 25) & 15
    if group in (8, 9):  # data-processing immediate, excluding ADR above
        fields = (word & 31, (word >> 5) & 31)
    elif group in (4, 5, 6, 7, 12, 13, 14, 15):
        fields = (word & 31, (word >> 5) & 31, (word >> 10) & 31, (word >> 16) & 31)
    else:
        raise ValueError(f'unsupported entry instruction {word:#010x}')
    if 18 in fields:
        raise ValueError(f'possible untranslated x18 instruction {word:#010x}')
    return [word]


def name16(name):
    return name.encode().ljust(16, b'\0')


def section(name, segment, va, size, offset, executable=False, alignment=3):
    return SECTION.pack(name16(name), name16(segment), va, size, offset,
                        alignment, 0, 0, 0x80000400 if executable else 0, 0, 0, 0)


def segment(name, va, vm_size, file_offset, file_size, prot, sections):
    size = SEGMENT.size + sum(map(len, sections))
    return SEGMENT.pack(0x19, size, name16(name), va, vm_size,
                        file_offset, file_size, prot, prot, len(sections), 0) + b''.join(sections)


def prepare(elf, macho, manifest):
    if manifest.get('schema') != 1:
        raise ValueError('unsupported hook manifest schema')
    for label, data in [('elf', elf), ('translation', macho)]:
        if hashlib.sha256(data).hexdigest() != manifest.get(label + '_sha256'):
            raise ValueError(label + ' SHA-256 mismatch')
    if len(elf) < 64 or elf[:6] != b'\x7fELF\x02\x01' or struct.unpack_from('<H', elf, 18)[0] != 183:
        raise ValueError('expected ARM64 ELF64 little-endian source')
    if len(macho) < 32:
        raise ValueError('truncated Mach-O')
    header = list(struct.unpack_from('<8I', macho))
    if header[:4] != [0xfeedfacf, 0x100000c, 0, 6]:
        raise ValueError('expected thin ARM64 klepton-ld dylib')
    commands, segments = [], []
    cursor = 32
    elf_section = None
    for _ in range(header[4]):
        command, size = struct.unpack_from('<II', macho, cursor)
        if size < 8 or cursor + size > 32 + header[5]:
            raise ValueError('invalid Mach-O load command')
        data = bytearray(macho[cursor:cursor+size])
        if command == 0x1d:
            raise ValueError('input already signed; prepare hooks before codesign')
        if command == 0x19:
            seg = SEGMENT.unpack_from(data)
            segname = seg[2].rstrip(b'\0')
            if segname.startswith(b'__SC2'):
                raise ValueError('input already has hook segments')
            segments.append(seg)
            for i in range(seg[9]):
                sec = SECTION.unpack_from(data, SEGMENT.size + i * SECTION.size)
                if sec[0].rstrip(b'\0') == b'__klelf':
                    elf_section = sec
        commands.append((command, data))
        cursor += size
    if cursor != 32 + header[5] or not elf_section:
        raise ValueError('invalid command size or missing __klelf')
    if not any(s[2].rstrip(b'\0') == b'__TEXT' and s[3] == 0 and s[5] == 0 for s in segments):
        raise ValueError('only zero-based klepton-ld layouts supported')
    shift = elf_section[2]

    def at_va(va, size, executable=False):
        for seg in segments:
            if seg[3] <= va and va + size <= seg[3] + seg[6] and (not executable or seg[8] == 5):
                return seg[5] + va - seg[3]
        raise ValueError(f'address {va:#x} not in a qualifying file-backed segment')

    phoff = struct.unpack_from('<Q', elf, 32)[0]
    phsize, phnum = struct.unpack_from('<HH', elf, 54)
    if phsize != 56 or phoff + phnum * 56 > len(elf):
        raise ValueError('invalid ELF program headers')
    phdrs = [struct.unpack_from('<IIQQQQQQ', elf, phoff+i*56) for i in range(phnum)]
    shoff=struct.unpack_from('<Q',elf,40)[0]
    shsize,shnum=struct.unpack_from('<HH',elf,58)
    if shsize!=64 or not shnum or shoff+shnum*64>len(elf):
        raise ValueError('valid ELF section headers are required to identify code')
    sections=[struct.unpack_from('<IIQQQQIIQQ',elf,shoff+i*64) for i in range(shnum)]
    if min(p[3] for p in phdrs if p[0] == 1) != 0:
        raise ValueError('only zero-based guest ELF layouts supported')
    targets = manifest.get('targets', [])
    if not 0 < len(targets) <= 1000000:
        raise ValueError('empty or excessive target list')
    targets = sorted(targets, key=lambda t: int(t['vaddr'], 0) if isinstance(t['vaddr'], str) else t['vaddr'])
    selected = []
    for target in targets:
        va = int(target['vaddr'], 0) if isinstance(target['vaddr'], str) else target['vaddr']
        if va % 4 or va < 0 or (selected and va == selected[-1][0]):
            raise ValueError('unaligned or duplicate target')
        load = next((p for p in phdrs if p[0] == 1 and p[1] & 1 and p[3] <= va and va+4 <= p[3]+p[5]), None)
        if not load:
            raise ValueError('target is not executable ELF file data')
        if not any(s[1]==1 and s[2]&4 and s[3]<=va and va+4<=s[3]+s[5] for s in sections):
            raise ValueError('target is not in an executable ELF section')
        raw_at = load[2] + va - load[3]
        expected = int(target['expect'], 0)
        if struct.unpack_from('<I', elf, raw_at)[0] != expected:
            raise ValueError('original entry instruction mismatch')
        address = shift + va
        file_at = at_va(address, 4, True)
        word = struct.unpack_from('<I', macho, file_at)[0]
        if word != int(target.get('translated_expect', target['expect']), 0):
            raise ValueError('translated entry differs; explicitly bind the translated instruction')
        selected.append((va, address, file_at, word, target.get('name', hex(va))))
    link = next((s for s in segments if s[2].rstrip(b'\0') == b'__LINKEDIT'), None)
    if not link or link[5] + link[6] != len(macho):
        raise ValueError('expected terminal klepton-ld __LINKEDIT')
    # The translator emits no bindings/exports/rebases and an empty symbol table.
    for command, data in commands:
        if command == 2 and (struct.unpack_from('<I', data, 12)[0] != 0 or
                             struct.unpack_from('<II',data,16)!=(link[5],link[6]) or any(macho[link[5]:])):
            raise ValueError('only empty klepton-ld symbol tables supported')
        if command in (0x80000022, 0x22, 0x80000033, 0x80000034):
            raise ValueError('unexpected dyld linkedit payload')

    prepare_originals=manifest.get('prepareOriginalEntries',False)
    if not isinstance(prepare_originals,bool): raise ValueError('prepareOriginalEntries must be boolean')
    original_depth=manifest.get('originalEntryDepth',1 if prepare_originals else 0)
    if type(original_depth)!=int or not 0<=original_depth<=8: raise ValueError('originalEntryDepth must be an integer from 0 to 8')
    entry_count=len(selected)*(original_depth+1)
    if entry_count>1000000: raise ValueError('excessive prepared entry count')
    code_va, code_off = align(link[3]), align(link[5])
    code_size = entry_count * 48  # 16-byte gate and bounded relocation + B
    records_at = align(code_size, 8)
    records_size = HEADER.size + entry_count * ENTRY.size
    rx_size = records_at + records_size
    data_va = align(code_va + rx_size)
    data_off = align(code_off + rx_size)
    slots_size = entry_count * 8
    link_va = align(data_va + slots_size)
    link_off = align(data_off + slots_size)
    output = bytearray(link_off + link[6])
    output[:link[5]] = macho[:link[5]]
    output[link_off:] = macho[link[5]:]
    records = bytearray(HEADER.pack(MAGIC,1,ENTRY.size,entry_count,0,shift,bytes.fromhex(manifest['elf_sha256'])))
    report = []
    rows=list(selected)
    for i, (va, address, file_at, word, name) in enumerate(rows):
        gate = code_va + i * 48
        original = gate + 16
        slot = data_va + i * 8
        gate_words = [adrp(gate,slot,16), add_address(16,slot), 0xc8dffe10, 0xd61f0200] # LDAR x16,[x16]; BR x16
        relocated = relocate(word,address,original)
        relocated.append(branch(original+len(relocated)*4,address+4))
        words = gate_words + relocated
        if len(words) * 4 > 48:
            raise ValueError('original trampoline exceeds reserved record')
        struct.pack_into('<'+'I'*len(words),output,code_off+i*48,*words)
        patched = branch(address,gate)
        struct.pack_into('<I',output,file_at,patched)
        records.extend(ENTRY.pack(address,original,slot,gate,patched,word))
        report.append({'name':name,'elfVaddr':hex(va),'gate':hex(gate),'original':hex(original),'slot':hex(slot),'entryWord':hex(word)})
        if i<len(selected)*original_depth:
            # The SDK's InstallOrigHook hooks an existing original-call
            # trampoline. Give that exact entry its own signed dispatch gate.
            # Relocating its first word again preserves the remaining original
            # sequence and its original PCs; no writable code is introduced.
            rows.append((original-shift,original,code_off+i*48+16,relocated[0],name+'::original-entry'))
    output[code_off+records_at:code_off+records_at+records_size] = records
    new_segments = [segment('__SC2HOOK',code_va,align(rx_size),code_off,rx_size,5,[
        section('__gates','__SC2HOOK',code_va,code_size,code_off,True,4),
        section('__records','__SC2HOOK',code_va+records_at,records_size,code_off+records_at)]),
        segment('__SC2DATA',data_va,align(slots_size),data_off,slots_size,3,[
        section('__slots','__SC2DATA',data_va,slots_size,data_off)])]
    revised = []
    for command, data in commands:
        if command == 0x19 and data[8:24].rstrip(b'\0') == b'__LINKEDIT':
            revised.extend(new_segments)
            struct.pack_into('<Q',data,24,link_va)
            struct.pack_into('<Q',data,40,link_off)
        if command == 2:
            struct.pack_into('<I',data,16,link_off)
        if command == 0x1b:
            data[8:24] = hashlib.sha256(macho+records).digest()[:16]
        revised.append(data)
    cmds = b''.join(revised)
    if 32 + len(cmds) > 0x3fc0:
        raise ValueError('hook load commands overlap translator header reserve')
    header[4] += 2
    header[5] = len(cmds)
    struct.pack_into('<8I',output,0,*header)
    output[32:32+len(cmds)] = cmds
    return bytes(output), {'schema':1,'elfSha256':manifest['elf_sha256'],
        'inputTranslationSha256':manifest['translation_sha256'],'originalEntryDepth':original_depth,'targets':report,
        'scope':'Signed data-dispatch hooks; explicitly selected entry points only; no runtime code writes.'}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--elf',type=Path,required=True)
    parser.add_argument('--input',type=Path,required=True)
    parser.add_argument('--manifest',type=Path,required=True)
    parser.add_argument('--output',type=Path,required=True)
    args = parser.parse_args()
    report_path=args.output.with_suffix('.hooks.json')
    inputs={args.input.resolve(),args.elf.resolve(),args.manifest.resolve()}
    if any(path.is_symlink() or path.resolve() in inputs for path in (args.output,report_path)):
        parser.error('output and report must be distinct from every input and must not be symbolic links')
    try:
        result, report = prepare(args.elf.read_bytes(),args.input.read_bytes(),json.loads(args.manifest.read_text()))
    except (ValueError,KeyError,struct.error,StopIteration) as error:
        parser.error(str(error))
    args.output.write_bytes(result)
    args.output.chmod(0o755)
    report_path.write_text(json.dumps(report,indent=2)+'\n')
    print(f'Prepared {len(report["targets"])} signed hook entries: {args.output}')


if __name__ == '__main__':
    main()
