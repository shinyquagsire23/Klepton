#!/usr/bin/env python3
"""Build the pinned original BSML, its assets, gif-lib and parser dependencies."""
import concurrent.futures,hashlib,json,os,re,shutil,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];os.chdir(ROOT)
src=ROOT/'ports/scotland2/bsml/upstream';out=ROOT/'build/scotland2/android-bsml'
sdk=ROOT/'build/scotland2/mod-sdk';dep=ROOT/'port-deps/sources'
il2cpp=dep/'libil2cpp/il2cpp'
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
bin=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin'
cc=bin/'aarch64-linux-android26-clang++';c=bin/'aarch64-linux-android26-clang'
out.mkdir(parents=True,exist_ok=True);(out/'objects').mkdir(exist_ok=True);(out/'deps').mkdir(exist_ok=True)
for name in ['kaleb','gif-lib','tinyxml2']:
    link=sdk/name;target=dep/name
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
for row in json.loads((dep/'bsml-submodules-lock.json').read_text()):
    name=row['directory'];link=out/'include/submodules'/name;target=dep/name
    link.parent.mkdir(parents=True,exist_ok=True)
    if link.is_dir() and not link.is_symlink() and not any(link.iterdir()):link.rmdir()
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
    archive=dep/(name+'-'+row['commit'][:7]+'.tar.gz')
    assert hashlib.sha256(archive.read_bytes()).hexdigest()==row['sha256']
for row in json.loads((dep/'bsml-extra-lock.json').read_text()):
    assert hashlib.sha256((dep/row['archive']).read_bytes()).hexdigest()==row['sha256']
(out/'git_info.h').write_text('#pragma once\n#define GIT_USER "Scotland2 port"\n#define GIT_BRANCH "v0.4.55"\n#define GIT_COMMIT 0x4987a88\n#define GIT_MODIFIED 1\n')
includes=[out,out/'include',src,src/'src',src/'include',src/'shared',sdk,dep/'bs-cordl/include',
    il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
    dep/'fmt/include',dep/'utfcpp/source',dep/'css-color-parser-cpp',dep/'EasyGifReader',
    dep/'cppcodec',dep/'gif-lib/shared',dep/'gif-lib/include',dep/'gif-lib/src']
assert all(p.is_dir() for p in includes)
flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fvisibility=hidden',
    '-fdeclspec','-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY',
    '-DSC2_AOT_HOOKS','-DCORDL_RUNTIME_FIELD_NULL_CHECKS','-DKALEB_VERSION="0.1.9"',
    '-DVERSION="0.4.55"','-DMOD_ID="bsml"','-DMAKE_DOCS=0',
    '-Wno-invalid-offsetof','-Wno-extra-qualification',
    '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [arg for p in includes for arg in ['-I',str(p)]]
sources=sorted((src/'src').rglob('*.cpp'))+[dep/'css-color-parser-cpp/csscolorparser.cpp',dep/'EasyGifReader/EasyGifReader.cpp']
digest=hashlib.sha256(json.dumps(flags).encode())
for path in sorted(p for p in src.rglob('*') if p.is_file() and p.suffix!='.cpp'):digest.update(path.read_bytes())
for p in [ROOT/'ports/scotland2/bshook-sdk.patch',ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp',
    *(dep/n for n in ['bsml-submodules-lock.json','bsml-extra-lock.json','bs-cordl-lock.json','libil2cpp-lock.json','sdk-submodules-lock.json'])]:digest.update(p.read_bytes())
stamp=digest.hexdigest();state=out/'build-key.txt'
fresh=state.exists() and state.read_text()==stamp and os.environ.get('SC2_REBUILD')!='1'
jobs=max(1,min(4,int(os.environ.get('SC2_JOBS','2'))))
def object_path(source):return out/'objects'/(str(source.relative_to(ROOT)).replace('/','__')+'.o')
def compile_one(source):
    obj=object_path(source)
    key=hashlib.sha256(stamp.encode()+source.read_bytes()).hexdigest();keyfile=obj.with_suffix('.key')
    if os.environ.get('SC2_REBUILD')!='1' and obj.exists() and keyfile.exists() and keyfile.read_text()==key:return source,0
    print('Compiling',source.relative_to(ROOT),flush=True)
    with obj.with_suffix('.log').open('w') as log:
        result=subprocess.run([str(cc),*flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
    if not result.returncode:keyfile.write_text(key)
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
# Same Kaleb layout as its original CMake rule: 32 writable header bytes,
# unchanged asset bytes, and one extra byte made NUL by Kaleb's constructor.
asm=['.section .data.sc2_assets,"aw",@progbits']
for asset in sorted((src/'assets').rglob('*')):
    if not asset.is_file():continue
    name='_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',str(asset.relative_to(src/'assets')))
    asm += ['.balign 8','.global '+name+'_start','.global '+name+'_end',name+'_start:',
        '.space 32,32','.incbin '+json.dumps(str(asset)),'.byte 32',name+'_end:']
(out/'assets.S').write_text('\n'.join(asm)+'\n')
subprocess.run([str(c),'-c',str(out/'assets.S'),'-o',str(out/'assets.o')],check=True)
gif_objects=[]
for source in sorted((dep/'gif-lib/src').glob('*.c')):
    obj=out/'objects'/('gif_'+source.stem+'.o');gif_objects.append(obj)
    subprocess.run([str(c),'-O2','-fPIC','-fvisibility=hidden',
        '-I'+str(dep/'gif-lib/shared'),'-I'+str(dep/'gif-lib/include'),'-I'+str(dep/'gif-lib/src'),
        '-c',str(source),'-o',str(obj)],check=True)
lock=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
xml,=[b for b in lock['binaries'] if b['mod']=='tinyxml2']
original=ROOT/'port-results/coremods'/xml['file'];assert hashlib.sha256(original.read_bytes()).hexdigest()==xml['sha256']
shutil.copy2(original,out/'deps/libtinyxml2.so')
subprocess.run([str(cc),'-shared',*[str(object_path(s)) for s in sources],*[str(x) for x in gif_objects],str(out/'assets.o'),
    '-Lbuild/scotland2/android-aot','-lsl2_abi','-Lbuild/scotland2/android-customtypes','-lcustom-types',
    '-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2',
    '-L'+str(out/'deps'),'-ltinyxml2','-llog','-lz','-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL',
    '-Wl,-soname,libbsml.so','-o',str(out/'libbsml.debug.so')],check=True)
subprocess.run([str(bin/'llvm-objcopy'),'--strip-debug',str(out/'libbsml.debug.so'),str(out/'libbsml.so')],check=True)
state.write_text(stamp)
subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py','--mod',str(out/'libbsml.so'),
    '--game','beatsaber140/lib/arm64-v8a/libil2cpp.so','--output',str(out/'hook-catalog.json')],check=True)
catalog=json.loads((out/'hook-catalog.json').read_text())
(out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
record=json.loads((ROOT/'ports/scotland2/bsml/provenance.json').read_text());record['buildKey']=stamp
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf
elf=Elf(str(out/'libbsml.so'))
released,=[b for b in lock['binaries'] if b['mod']=='bsml']
original=Elf(str(ROOT/'port-results/coremods'/released['file']))
missing=sorted(x for x in original.exports-elf.exports if 'BSML' in x)
assert not missing,missing
imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mprotect','mmap'}&imports
assert not any(x.startswith('A64') for x in elf.exports)
record.update(originalBSMLExportsPreserved=True,importsAdaptedAnd64=True,noExecutableMemoryImports=True)
record['scope']='Original BSML source, assets and pinned parser/GIF dependencies built with compiler hook catalog and imported signed And64 backend. Runtime callbacks not verified by compilation.'
record['files']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [
    out/'libbsml.so',out/'hook-catalog.json',ROOT/'ports/scotland2/bsml/upstream.patch']}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built actual BSML source:',out/'libbsml.so')
