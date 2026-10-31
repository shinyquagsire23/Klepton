#!/usr/bin/env python3
"""Read compiler-emitted mod hook addresses and bind them to original game code."""
import argparse,hashlib,json,struct,sys
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[3]/'tools'))
from audit_elf import Elf,cstr

def read(mod_path,game_path):
    mod=Elf(str(mod_path));game=Elf(str(game_path))
    shstr=mod.sh[struct.unpack_from('<H',mod.data,62)[0]][4]
    sections=[s for s in mod.sh if cstr(mod.data,shstr+s[0])=='.sc2_hooks']
    if len(sections)!=1:raise ValueError('expected one compiler hook catalog')
    s,=sections
    if s[1]!=1 or not s[2]&2 or s[2]&1 or not s[5] or s[5]%144:
        raise ValueError('invalid or writable hook catalog')
    rows=[];group={}
    for at in range(s[4],s[4]+s[5],144):
        address,original,reserved,name=struct.unpack_from('<QII128s',mod.data,at)
        if original not in (0,1) or reserved or b'\0' not in name:raise ValueError('invalid compiler hook record')
        name=name.split(b'\0')[0].decode('ascii')
        if not name or not address or address%4:raise ValueError('invalid hook name/address')
        off=game._off(address)
        if off is None or not any(x[1]==1 and x[2]&4 and x[3]<=address<=x[3]+x[5]-4 for x in game.sh):
            raise ValueError('hook is not executable game code: '+name)
        row={'name':name,'vaddr':hex(address),'expect':hex(struct.unpack_from('<I',game.data,off)[0]),'originalHook':bool(original)}
        rows.append(row);group.setdefault(address,[]).append(row)
    targets=[{'name':' / '.join(x['name'] for x in values),'vaddr':hex(address),'expect':values[0]['expect']} for address,values in sorted(group.items())]
    return {'schema':1,'modSha256':hashlib.sha256(mod.data).hexdigest(),'gameElfSha256':hashlib.sha256(game.data).hexdigest(),
        'catalog':rows,'targets':targets,'originalEntryDepth':max(2,max(sum(x['originalHook'] for x in values) for values in group.values())),
        'scope':'Compiler-resolved generated-header addresses; runtime method lookup must still match prepared targets. Not proof of installed hooks.'}

if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--mod',type=Path,required=True);p.add_argument('--game',type=Path,required=True);p.add_argument('--output',type=Path,required=True)
    a=p.parse_args();d=read(a.mod,a.game);a.output.write_text(json.dumps(d,indent=2)+'\n')
    print(len(d['catalog']),'hook uses,',len(d['targets']),'unique targets, original-entry depth',d['originalEntryDepth'])
