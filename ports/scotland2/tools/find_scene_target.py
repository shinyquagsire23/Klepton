#!/usr/bin/env python3
"""Find this game's scene hook using Scotland2's original DestroyImmediate path.

Locate the exact icall registration string/code, then follow the original
last-BL-before-first-RET / first-B lookup. Bind all results to the input ELF.
Runtime independently repeats the lookup through the actual icall resolver.
"""
import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
from audit_elf import Elf
from prepare_hooks import signed


def find(path):
    elf=Elf(str(path))
    # Scotland2 0.1.7 uses this same fallback for Unity 6's injected icall.
    names=[b'UnityEngine.Object::DestroyImmediate\0',b'UnityEngine.Object::DestroyImmediate_Injected\0']
    needle=next((name for name in names if name in elf.data),None)
    if needle is None:raise ValueError('Neither original Scotland2 scene icall is registered')
    off=elf.data.index(needle)
    string=next(s[3]+off-s[4] for s in elf.sh if s[1]==1 and s[4]<=off<s[4]+s[5])
    references=[]
    for s in elf.sh:
        if s[1]!=1 or not s[2]&4: continue
        for index,(word,) in enumerate(struct.iter_unpack('<I',elf.data[s[4]:s[4]+s[5]//4*4])):
            pc=s[3]+index*4
            if word&0x9f00001f!=0x90000000: continue # ADRP x0
            page=(pc&-4096)+signed(((word>>5)&0x7ffff)*4+((word>>29)&3),21)*4096
            if page!=(string&-4096): continue
            regs={0:page}
            for step in range(1,9):
                if (index+step)*4+4>s[5]: break
                w=struct.unpack_from('<I',elf.data,s[4]+(index+step)*4)[0];at=pc+step*4
                if w&0x9f000000==0x90000000:
                    regs[w&31]=(at&-4096)+signed(((w>>5)&0x7ffff)*4+((w>>29)&3),21)*4096
                elif w&0x9f000000==0x10000000: # linker-relaxed ADR, used by Unity 6.3 for the function pointer
                    regs[w&31]=at+signed(((w>>5)&0x7ffff)*4+((w>>29)&3),21)
                elif w&0xffc00000==0x91000000 and ((w>>5)&31) in regs:
                    regs[w&31]=regs[(w>>5)&31]+((w>>10)&4095)
                elif w&0x7c000000==0x14000000:
                    if regs.get(0)==string and 1 in regs: references.append((pc,regs[1]))
                    break
    if len(references)!=1: raise ValueError(f'expected one exact registration, found {references}')

    def word(va):
        offset=elf._off(va)
        if offset is None: raise ValueError('branch outside ELF file')
        if not any(s[1]==1 and s[2]&4 and s[3]<=va< s[3]+s[5] for s in elf.sh):
            raise ValueError('branch outside executable section')
        return struct.unpack_from('<I',elf.data,offset)[0]
    registration,destroy=references[0]
    ret=next((destroy+i*4 for i in range(100) if word(destroy+i*4)==0xd65f03c0),None)
    if ret is None: raise ValueError('no bounded DestroyImmediate RET')
    call=next((pc for pc in range(ret,destroy,-4) if word(pc)>>26==0b100101),None)
    if call is None: raise ValueError('no final direct BL before RET')
    scripting=call+signed(word(call)&0x3ffffff,26)*4
    branch=next((scripting+i*4 for i in range(1024) if word(scripting+i*4)>>26==0b000101),None)
    if branch is None: raise ValueError('no bounded scripting-destroy B')
    target=branch+signed(word(branch)&0x3ffffff,26)*4
    first=word(target)
    return {'schema':1,'elf_sha256':hashlib.sha256(elf.data).hexdigest(),
            'icall':needle[:-1].decode(),
            'lookup':'Original Scotland2 DestroyImmediate -> last BL before first RET -> first B',
            'registration':hex(registration),'destroyImmediate':hex(destroy),'lastCall':hex(call),
            'scriptingDestroyImmediate':hex(scripting),'firstBranch':hex(branch),
            'target':{'name':'DestroyObjectHighLevel','vaddr':hex(target),'expect':hex(first)}}


if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument('--elf',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args()
    try: record=find(a.elf)
    except (ValueError,StopIteration) as e: p.error(str(e))
    a.output.write_text(json.dumps(record,indent=2)+'\n')
    print('Original Scotland2 scene target:',record['target']['vaddr'])
