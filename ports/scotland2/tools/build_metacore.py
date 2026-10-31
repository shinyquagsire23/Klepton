#!/usr/bin/env python3
"""Build pinned MetaCore sources and retain compiler-resolved hook target metadata."""
import concurrent.futures,hashlib,json,os,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
os.chdir(ROOT)
src=ROOT/'ports/scotland2/metacore/upstream';out=ROOT/'build/scotland2/android-metacore'
sdk=ROOT/'build/scotland2/mod-sdk';il2cpp=ROOT/'port-deps/sources/libil2cpp/il2cpp'
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
cc=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++'
out.mkdir(parents=True,exist_ok=True);(out/'objects').mkdir(exist_ok=True)
for name,target in [('custom-types',ROOT/'ports/scotland2/customtypes/upstream'),('song-details',ROOT/'ports/scotland2/song-details/upstream'),('web-utils',ROOT/'ports/scotland2/web-utils/upstream'),('rapidjson-macros',ROOT/'port-deps/sources/rapidjson-macros')]:
    link=sdk/name
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
includes=[src,src/'src',src/'include',src/'shared',sdk,ROOT/'port-deps/sources/bs-cordl/include',
    il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
    ROOT/'port-deps/sources/fmt/include',ROOT/'port-deps/sources/utfcpp/source']
assert all(p.is_dir() for p in includes)
flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fvisibility=hidden','-fdeclspec',
    '-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS',
    '-DWEBUTILS_NO_BSML','-DVERSION="1.6.2"','-DVERSION_LONG=0','-DMOD_ID="metacore"','-Wno-invalid-offsetof','-Wno-extra-qualification',
    '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [arg for path in includes for arg in ['-I',str(path)]]
unity_profile_path=ROOT/'ports/scotland2/metacore/unity-target-profile.json'
unity_profile=json.loads(unity_profile_path.read_text())
delete_offset=int(unity_profile['target']['vaddr'],0)
flags.append('-DSC2_METACORE_DELETE_OFFSET='+hex(delete_offset))
adapter=ROOT/'ports/scotland2/src/android-metacore-probe.cpp'
sources=sorted((src/'src').rglob('*.cpp'))+[adapter]
# Hash the working mod and port adapters. Pinned SDK archives are recorded in
# the lock files; SC2_REBUILD=1 forces recompilation after manual SDK changes.
digest=hashlib.sha256(json.dumps(flags).encode())
for path in sorted(p for p in src.rglob('*') if p.is_file()):digest.update(path.read_bytes())
for name in ['bshook-sdk.patch','src/android-mod-sdk-prelude.hpp']:
    digest.update((ROOT/'ports/scotland2'/name).read_bytes())
for name in ['bs-cordl-lock.json','libil2cpp-lock.json','sombrero-lock.json','lock.json','sdk-submodules-lock.json']:
    digest.update((ROOT/'port-deps/sources'/name).read_bytes())
digest.update(adapter.read_bytes())
digest.update(unity_profile_path.read_bytes())
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
link=[str(cc),'-shared',*[str(object_path(s)) for s in sources],
    '-Lbuild/scotland2/android-aot','-lsl2_abi','-Lbuild/scotland2/android-customtypes','-lcustom-types',
    '-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2','-Lbuild/scotland2/android-webutils','-lweb-utils',
    '-L'+str(ROOT/'build/scotland2/android-song-details' if (ROOT/'build/scotland2/android-song-details/libsongdetails.so').is_file() else ROOT/'port-results/coremods/elf/song-details/1.0.4'),'-lsongdetails','-llog',
    '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,libmetacore.so','-o',str(out/'libmetacore.so')]
subprocess.run(link,check=True)
debug=out/'libmetacore.debug.so';(out/'libmetacore.so').replace(debug)
subprocess.run([str(cc.parent/'llvm-objcopy'),'--strip-debug',str(debug),str(out/'libmetacore.so')],check=True)
state.write_text(stamp)
subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py',
    '--mod',str(out/'libmetacore.so'),'--game','beatsaber140/lib/arm64-v8a/libil2cpp.so',
    '--output',str(out/'hook-catalog.json')],check=True)
catalog=json.loads((out/'hook-catalog.json').read_text())
(out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf
elf=Elf(str(out/'libmetacore.so'))
# Two manual hooks are separate from the IL2CPP compiler catalog.
unity=Elf(str(ROOT/'beatsaber140/lib/arm64-v8a/libunity.so'))
manual=re.findall(r'baseAddr\("libunity.so"\) \+ (0x[0-9a-fA-F]+)',(src/'src/hooks.cpp').read_text())
assert len(manual)==1
assert int(manual[0],0)==0x8d2898 # retained original fallback, never used by this build
address=delete_offset;offset=unity._off(address)
assert hashlib.sha256(unity.data).hexdigest()==unity_profile['unitySha256']
for i,expected in enumerate(unity_profile['entryWords']):
    assert struct.unpack_from('<I',unity.data,unity._off(address+i*4))[0]==int(expected,0)
for anchor in unity_profile.get('bodyWords',[]):
    assert struct.unpack_from('<I',unity.data,unity._off(int(anchor['vaddr'],0)))[0]==int(anchor['expect'],0)
for call in unity_profile['callsites']:
    pc=int(call['vaddr'],0);word=struct.unpack_from('<I',unity.data,unity._off(pc))[0]
    assert word==int(call['expect'],0) and word&0x7c000000==0x14000000
    delta=word&0x3ffffff
    if delta&(1<<25):delta-=1<<26
    assert pc+delta*4==address
assert offset is not None and any(x[1]==1 and x[2]&4 and x[3]<=address<=x[3]+x[5]-4 for x in unity.sh)
runtime={'unity':{'sha256':hashlib.sha256(unity.data).hexdigest(),'targets':[{'name':'delete_object_internal_step1','vaddr':hex(address),'expect':hex(struct.unpack_from('<I',unity.data,offset)[0])}]},'nativeAbortGateRequired':True}
(out/'runtime-targets.json').write_text(json.dumps(runtime,indent=2)+'\n')
lock=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
released,=[b for b in lock['binaries'] if b['mod']=='metacore']
original=Elf(str(ROOT/'port-results/coremods'/released['file']))
missing=sorted(x for x in original.exports-elf.exports if 'MetaCore' in x)
assert not missing,missing
imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mprotect','mmap'}&imports
assert not any(x.startswith('A64') for x in elf.exports)
record=json.loads((ROOT/'ports/scotland2/metacore/provenance.json').read_text())
record['buildKey']=stamp
record['originalMetaCoreExportsPreserved']=True
record['importsAdaptedAnd64']=True
record['noExecutableMemoryImports']=True
record['scope']='Actual MetaCore source compiled with static hook catalog; native abort and Unity manual hooks require platform preparation before real late callback. Not yet late-tested.'
record['files']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
    adapter,unity_profile_path,out/'runtime-targets.json',out/'libmetacore.so',out/'hook-catalog.json',out/'hook-profile.txt',ROOT/'ports/scotland2/metacore/upstream.patch',
    ROOT/'ports/scotland2/bshook-sdk.patch']}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built actual MetaCore source:',out/'libmetacore.so')
