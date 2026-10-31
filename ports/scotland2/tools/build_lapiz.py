#!/usr/bin/env python3
"""Build pinned Lapiz sources and retain compiler-resolved hook target metadata."""
import concurrent.futures,hashlib,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
os.chdir(ROOT)
src=ROOT/'ports/scotland2/lapiz/upstream';out=ROOT/'build/scotland2/android-lapiz'
sdk=ROOT/'build/scotland2/mod-sdk';il2cpp=ROOT/'port-deps/sources/libil2cpp/il2cpp'
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
cc=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++'
out.mkdir(parents=True,exist_ok=True);(out/'objects').mkdir(exist_ok=True)
for name,target in [('custom-types',ROOT/'ports/scotland2/customtypes/upstream'),('sombrero',ROOT/'port-deps/sources/sombrero')]:
    link=sdk/name
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
includes=[src,src/'src',src/'include',src/'shared',sdk,ROOT/'port-deps/sources/bs-cordl/include',
    il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
    ROOT/'port-deps/sources/fmt/include',ROOT/'port-deps/sources/utfcpp/source']
assert all(p.is_dir() for p in includes)
flags=['-std=c++20','-O1','-g','-fPIC','-frtti','-fexceptions','-fvisibility=hidden','-fdeclspec',
    '-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS',
    '-DVERSION="0.2.23"','-DVERSION_LONG=0','-DMOD_ID="lapiz"','-Wno-invalid-offsetof','-Wno-extra-qualification',
    '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [arg for path in includes for arg in ['-I',str(path)]]
sources=sorted(src.rglob('*.cpp'))
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
def object_path(source):return out/'objects'/(str(source.relative_to(src)).replace('/','__')+'.o')
def compile_one(source):
    obj=object_path(source)
    if fresh and obj.exists():return source,0
    print('Compiling',source.relative_to(src),flush=True)
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
    '-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2','-llog',
    '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,liblapiz.so','-o',str(out/'liblapiz.so')]
subprocess.run(link,check=True)
debug=out/'liblapiz.debug.so';(out/'liblapiz.so').replace(debug)
subprocess.run([str(cc.parent/'llvm-objcopy'),'--strip-debug',str(debug),str(out/'liblapiz.so')],check=True)
state.write_text(stamp)
subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py',
    '--mod',str(out/'liblapiz.so'),'--game','beatsaber140/lib/arm64-v8a/libil2cpp.so',
    '--output',str(out/'hook-catalog.json')],check=True)
catalog=json.loads((out/'hook-catalog.json').read_text())
(out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf
elf=Elf(str(out/'liblapiz.so'))
lock=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
released,=[b for b in lock['binaries'] if b['mod']=='lapiz']
original=Elf(str(ROOT/'port-results/coremods'/released['file']))
missing=sorted(x for x in original.exports-elf.exports if 'Lapiz' in x)
assert not missing,missing
imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mprotect','mmap'}&imports
assert not any(x.startswith('A64') for x in elf.exports)
record=json.loads((ROOT/'ports/scotland2/lapiz/provenance.json').read_text())
record['buildKey']=stamp
record['originalLapizExportsPreserved']=True
record['importsAdaptedAnd64']=True
record['noExecutableMemoryImports']=True
record['files']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
    out/'liblapiz.so',out/'hook-catalog.json',out/'hook-profile.txt',ROOT/'ports/scotland2/lapiz/upstream.patch',
    ROOT/'ports/scotland2/bshook-sdk.patch']}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built actual Lapiz source:',out/'liblapiz.so')
