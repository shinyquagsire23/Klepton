#!/usr/bin/env python3
"""Bind an explicit list of exported ELF functions to one exact translation."""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
from audit_elf import Elf, cstr


def generate(elf_path, translation, names):
    elf=Elf(str(elf_path))
    if not elf.ok:
        raise ValueError('expected AArch64 ELF64')
    symbols={}
    for table in elf.sh:
        if table[1]!=11: continue
        strings=elf.sh[table[6]][4]
        for offset in range(table[4],table[4]+table[5],24):
            name,info,other,index,address,size=struct.unpack_from('<IBBHQQ',elf.data,offset)
            if index and info&15==2:
                symbols[cstr(elf.data,strings+name)]=(address,size)
    targets=[]
    for name in names:
        if name not in symbols: raise ValueError('exported function absent: '+name)
        address,size=symbols[name]
        offset=elf._off(address)
        if offset is None or size<4: raise ValueError('invalid function extent: '+name)
        word=struct.unpack_from('<I',elf.data,offset)[0]
        targets.append({'name':name,'vaddr':hex(address),'expect':hex(word)})
    return {'schema':1,'elf_sha256':hashlib.sha256(elf.data).hexdigest(),
            'translation_sha256':hashlib.sha256(translation.read_bytes()).hexdigest(),'targets':targets}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf',type=Path,required=True)
    p.add_argument('--translation',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True)
    p.add_argument('symbols',nargs='+')
    a=p.parse_args()
    try: result=generate(a.elf,a.translation,a.symbols)
    except ValueError as e: p.error(str(e))
    a.output.write_text(json.dumps(result,indent=2)+'\n')
