#!/usr/bin/env python3
"""Build pinned PlaylistCore sources and retain compiler-resolved hook target metadata."""
import concurrent.futures,hashlib,json,os,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
os.chdir(ROOT)
src=ROOT/'ports/scotland2/playlistcore/upstream';out=ROOT/'build/scotland2/android-playlistcore'
sdk=ROOT/'build/scotland2/mod-sdk';il2cpp=ROOT/'port-deps/sources/libil2cpp/il2cpp'
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
cc=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++'
out.mkdir(parents=True,exist_ok=True);(out/'objects').mkdir(exist_ok=True)
for name,target in [('metacore',ROOT/'ports/scotland2/metacore/upstream'),('songcore',ROOT/'ports/scotland2/songcore/upstream'),('bsml',ROOT/'ports/scotland2/bsml/upstream'),('beatsaverplusplus',ROOT/'ports/scotland2/beatsaverplusplus/upstream'),('config-utils',ROOT/'port-deps/sources/config-utils')]:
    link=sdk/name
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
includes=[src,src/'src',src/'include',src/'shared',sdk,ROOT/'port-deps/sources/bs-cordl/include',
    il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
    ROOT/'port-deps/sources/fmt/include',ROOT/'port-deps/sources/utfcpp/source']
assert all(p.is_dir() for p in includes)
flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fdeclspec',
    '-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS',
    '-DVERSION="1.5.5"','-DVERSION_LONG=0','-DMOD_ID="playlistcore"','-Wno-invalid-offsetof','-Wno-extra-qualification',
    '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [arg for path in includes for arg in ['-I',str(path)]]
sources=sorted((src/'src').rglob('*.cpp'))
# Hash the working mod and port adapters. Pinned SDK archives are recorded in
# the lock files; SC2_REBUILD=1 forces recompilation after manual SDK changes.
digest=hashlib.sha256(json.dumps(flags).encode())
for path in sorted(p for p in src.rglob('*') if p.is_file()):digest.update(path.read_bytes())
for name in ['bshook-sdk.patch','src/android-mod-sdk-prelude.hpp']:
    digest.update((ROOT/'ports/scotland2'/name).read_bytes())
for name in ['bs-cordl-lock.json','libil2cpp-lock.json','sombrero-lock.json','lock.json','sdk-submodules-lock.json']:
    digest.update((ROOT/'port-deps/sources'/name).read_bytes())
stamp=digest.hexdigest();state=out/'build-key.txt'
fresh=state.exists() and state.read_text()==stamp and os.environ.get('SC2_REBUILD')!='1'
jobs=max(1,min(4,int(os.environ.get('SC2_JOBS','2'))))
def object_path(source):return out/'objects'/(str(source.relative_to(ROOT)).replace('/','__')+'.o')
def compile_one(source):
    obj=object_path(source)
    if fresh and obj.exists():return source,0
    print('Compiling',source.relative_to(ROOT),flush=True)
    with obj.with_suffix('.log').open('w') as log:
        result=subprocess.run([str(cc),*flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
    return source,result.returncode
remaining=iter(sources);failed=[]
with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as pool:
    pending={pool.submit(compile_one,s) for s in [next(remaining,None) for _ in range(jobs)] if s}
    while pending:
        done,pending=concurrent.futures.wait(pending,return_when=concurrent.futures.FIRST_COMPLETED)
        for future in done:
            source,code=future.result()
            if code:
                failed.append(source);print(object_path(source).with_suffix('.log').read_text(),file=sys.stderr)
        if not failed:
            for _ in done:
                source=next(remaining,None)
                if source:pending.add(pool.submit(compile_one,source))
if failed:sys.exit(1)
# PlaylistCore's original IncludedAsset convention includes 32 leading spaces
# and one trailing space; retain those writable header bytes exactly.
asm=['.section .data.sc2_assets,"aw",@progbits']
for asset in sorted((src/'assets').rglob('*')):
    if not asset.is_file():continue
    name='_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',str(asset.relative_to(src/'assets')))
    asm += ['.p2align 4','.global '+name+'_start',name+'_start:','.fill 32,1,32','.incbin '+json.dumps(str(asset)),'.byte 32','.global '+name+'_end',name+'_end:']
asm += ['.section .note.GNU-stack,"",%progbits']
(out/'assets.S').write_text('\n'.join(asm)+'\n')
subprocess.run([str(cc),'-c',str(out/'assets.S'),'-o',str(out/'assets.o')],check=True)
link=[str(cc),'-shared',*[str(object_path(s)) for s in sources],str(out/'assets.o'),
    '-Lbuild/scotland2/android-aot','-lsl2_abi','-Lbuild/scotland2/android-customtypes','-lcustom-types',
    '-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2',
    '-Lbuild/scotland2/android-webutils','-lweb-utils','-Lbuild/scotland2/android-bsml','-lbsml',
    '-Lbuild/scotland2/android-songcore','-lsongcore','-Lbuild/scotland2/android-metacore','-lmetacore',
    '-Lport-results/coremods/elf/beatsaverplusplus/0.2.1','-lbeatsaverplusplus','-llog',
    '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,libplaylistcore.so','-o',str(out/'libplaylistcore.so')]
subprocess.run(link,check=True)
debug=out/'libplaylistcore.debug.so';(out/'libplaylistcore.so').replace(debug)
subprocess.run([str(cc.parent/'llvm-objcopy'),'--strip-debug',str(debug),str(out/'libplaylistcore.so')],check=True)
state.write_text(stamp)
subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py',
    '--mod',str(out/'libplaylistcore.so'),'--game','beatsaber140/lib/arm64-v8a/libil2cpp.so',
    '--output',str(out/'hook-catalog.json')],check=True)
catalog=json.loads((out/'hook-catalog.json').read_text())
(out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf,cstr
elf=Elf(str(out/'libplaylistcore.so'))
lock=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
released,=[b for b in lock['binaries'] if b['mod']=='playlistcore']
original=Elf(str(ROOT/'port-results/coremods'/released['file']))
# Retain the actual exported API, not incidental compiler-emitted STL weak
# instantiations whose emission differs with optimization and the NDK version.
public=set()
for table in original.sh:
    if table[1]!=11:continue
    strings=original.sh[table[6]][4]
    for at in range(table[4],table[4]+table[5],24):
        name,info,vis,index,address,size=struct.unpack_from('<IBBHQQ',original.data,at)
        symbol=cstr(original.data,strings+name)
        if index and info>>4==1 and vis&3==0 and ('PlaylistCore' in symbol or symbol in ('setup','late_load')):
            public.add(symbol)
missing=sorted(public-elf.exports)
assert not missing,missing
imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mprotect','mmap'}&imports
assert not any(x.startswith('A64') for x in elf.exports)
record=json.loads((ROOT/'ports/scotland2/playlistcore/provenance.json').read_text())
record['buildKey']=stamp
record['originalPlaylistCoreExportsPreserved']=True
record['originalStrongPublicExportCount']=len(public)
record['importsAdaptedAnd64']=True
record['noExecutableMemoryImports']=True
record['scope']='Actual PlaylistCore source and original embedded asset compiled with eight static hook targets; runtime callbacks not established by compilation.'
record['files']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
    out/'libplaylistcore.so',out/'hook-catalog.json',out/'hook-profile.txt',ROOT/'ports/scotland2/playlistcore/upstream.patch',ROOT/'ports/scotland2/bshook-sdk.patch']}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built actual PlaylistCore source:',out/'libplaylistcore.so')
