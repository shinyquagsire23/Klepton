"""Reject wrong builds, unsupported relocation and malformed signed metadata."""
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import tempfile
import sys

ROOT=Path(__file__).resolve().parents[3]
spec=importlib.util.spec_from_file_location('prepare_hooks',ROOT/'ports/scotland2/tools/prepare_hooks.py')
hook=importlib.util.module_from_spec(spec);spec.loader.exec_module(hook)
out=ROOT/'build/scotland2/aot-hooks'
elf=(out/'libsc2_hook_fixture.so').read_bytes()
macho=(out/'unprepared.dylib').read_bytes()
manifest=json.loads((out/'manifest.json').read_text())

def sections(data):
    cursor=32;found={};segments=[]
    for _ in range(struct.unpack_from('<I',data,16)[0]):
        cmd,size=struct.unpack_from('<II',data,cursor)
        if cmd==0x19:
            seg=hook.SEGMENT.unpack_from(data,cursor);segments.append(seg)
            for i in range(seg[9]):
                sec=hook.SECTION.unpack_from(data,cursor+hook.SEGMENT.size+i*hook.SECTION.size)
                found[sec[0].rstrip(b'\0').decode()]=sec
        cursor+=size
    return found,segments

seen=[]
def rejects(name,e=elf,m=macho,record=None,contains=''):
    record=manifest if record is None else record
    try: hook.prepare(e,m,record)
    except (ValueError,struct.error) as error:
        assert contains in str(error),(name,str(error));seen.append(name)
    else: raise AssertionError('accepted '+name)

bad=copy.deepcopy(manifest);bad['elf_sha256']='00'*32
rejects('wrong-elf-hash',record=bad,contains='SHA-256')
bad=copy.deepcopy(manifest);bad['translation_sha256']='00'*32
rejects('wrong-translation-hash',record=bad,contains='SHA-256')
bad=copy.deepcopy(manifest);bad['targets'][0]['expect']='0x0'
rejects('wrong-original-instruction',record=bad,contains='instruction mismatch')
bad=copy.deepcopy(manifest);bad['targets'].append(copy.deepcopy(bad['targets'][0]))
rejects('duplicate-target',record=bad,contains='duplicate')
bad=copy.deepcopy(manifest);bad['targets'][0]['vaddr']='0x1'
rejects('unaligned-target',record=bad,contains='unaligned')
bad=copy.deepcopy(manifest);bad['targets'][0]['vaddr']='0xffffffff'
rejects('outside-target',record=bad,contains='unaligned')
sec,segs=sections(macho)
shift=sec['__klelf'][2]
address=int(manifest['targets'][0]['vaddr'],0)
phoff=struct.unpack_from('<Q',elf,32)[0];phnum=struct.unpack_from('<H',elf,56)[0]
loads=[struct.unpack_from('<IIQQQQQQ',elf,phoff+i*56) for i in range(phnum)]
p=next(p for p in loads if p[0]==1 and p[3]<=address<p[3]+p[5])
raw=p[2]+address-p[3]
s=next(s for s in segs if s[3]<=shift+address<s[3]+s[6])
translated=s[5]+shift+address-s[3]
for label,word in [('svc-entry',0xd4000001),('bl-needs-unwind',0x94000001),('reserved-x18',0x91000252),('ldr-x18',0xf9400012),('ldr-from-x18',0xf9400248)]:
    e=bytearray(elf);m=bytearray(macho)
    struct.pack_into('<I',e,raw,word);struct.pack_into('<I',m,translated,word)
    bad=copy.deepcopy(manifest);bad['elf_sha256']=hashlib.sha256(e).hexdigest();bad['translation_sha256']=hashlib.sha256(m).hexdigest();bad['targets'][0]['expect']=hex(word)
    rejects(label,e,m,bad)
prepared,report=hook.prepare(elf,macho,manifest)
bad=copy.deepcopy(manifest);bad['translation_sha256']=hashlib.sha256(prepared).hexdigest()
rejects('already-prepared',m=prepared,record=bad,contains='already has hook')
signed=(out/'libsc2_hook_fixture.dylib').read_bytes()
bad=copy.deepcopy(manifest);bad['translation_sha256']=hashlib.sha256(signed).hexdigest()
rejects('already-signed',m=signed,record=bad)

sec,_=sections(prepared)
records=sec['__records'][4];gates=sec['__gates'][4];slots=sec['__slots'][4]
faults=[('magic',records,'<B',0,'invalid or incompatible'),
        ('version',records+8,'<I',999,'invalid or incompatible'),
        ('entry-size',records+12,'<I',8,'invalid or incompatible'),
        ('entry-count',records+16,'<I',1000001,'invalid or incompatible'),
        ('elf-shift',records+24,'<Q',0,'invalid or incompatible'),
        ('slot-bounds',records+64+16,'<Q',0,'outside its permitted'),
        ('entry-branch',translated,'<I',0xd503201f,'branch mismatch'),
        ('gate-opcode',gates,'<I',0xd503201f,'gate instruction mismatch'),
        ('prefilled-slot',slots,'<Q',123,'not zero initialized')]
folder=out/'validation';folder.mkdir(exist_ok=True)
for name,offset,fmt,value,message in faults:
    with tempfile.TemporaryDirectory(prefix='sc2-aot-invalid-') as tmp:
        data=bytearray(prepared);struct.pack_into(fmt,data,offset,value)
        binary=Path(tmp)/'libsc2_hook_fixture.dylib';binary.write_bytes(data);binary.chmod(0o755)
        subprocess.run(['codesign','--force','--sign','-',str(binary)],check=True,capture_output=True)
        env=dict(os.environ,KL_DYLIB_DIR=tmp,KL_GUEST_JIT='0')
        result=subprocess.run([str(ROOT/'build/t_scotland2_aot'),str(out/'libsc2_hook_fixture.so')],env=env,capture_output=True,text=True)
        text=result.stdout+result.stderr;(folder/(name+'.log')).write_text(text)
        assert result.returncode==1 and message in text,(name,result.returncode,text)
        seen.append('signed-metadata-'+name)
# CLI output/report aliases must be rejected without modifying any input.
for kind in ['output-input-alias','report-input-alias','report-symlink']:
    with tempfile.TemporaryDirectory(prefix='sc2-aot-alias-') as tmp:
        tmp=Path(tmp);e=tmp/'fixture.so';m=tmp/'unprepared.dylib';record=tmp/'record.json';output=tmp/'output.dylib'
        e.write_bytes(elf);m.write_bytes(macho);record.write_text(json.dumps(manifest))
        if kind=='output-input-alias':output=m
        if kind=='report-input-alias':record=tmp/'output.hooks.json';record.write_text(json.dumps(manifest))
        if kind=='report-symlink':(tmp/'output.hooks.json').symlink_to(e)
        inputs={p:p.read_bytes() for p in (e,m,record)}
        result=subprocess.run([sys.executable,str(ROOT/'ports/scotland2/tools/prepare_hooks.py'),'--elf',str(e),'--input',str(m),'--manifest',str(record),'--output',str(output)],capture_output=True,text=True)
        assert result.returncode==2 and 'distinct' in result.stderr,(kind,result.stderr)
        assert all(p.read_bytes()==value for p,value in inputs.items())
        if kind!='output-input-alias':assert not output.exists()
        seen.append(kind)
(folder/'status.json').write_text(json.dumps({'passed':seen,'count':len(seen)},indent=2)+'\n')
print(f'PASS: {len(seen)} wrong-build, unsupported-instruction and malformed signed-record cases rejected before execution.')
