#!/usr/bin/env python3
"""Build pinned SongDownloader sources and retain compiler-resolved hook target metadata."""
import concurrent.futures,hashlib,json,os,re,struct,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
os.chdir(ROOT)
src=ROOT/'ports/scotland2/songdownloader/upstream';out=ROOT/'build/scotland2/android-songdownloader'
sdk=ROOT/'build/scotland2/mod-sdk';il2cpp=ROOT/'port-deps/sources/libil2cpp/il2cpp'
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
cc=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++'
out.mkdir(parents=True,exist_ok=True);(out/'objects').mkdir(exist_ok=True)
for name,target in [('playlistcore',ROOT/'ports/scotland2/playlistcore/upstream'),('songcore',ROOT/'ports/scotland2/songcore/upstream'),('bsml',ROOT/'ports/scotland2/bsml/upstream'),('beatsaverplusplus',ROOT/'ports/scotland2/beatsaverplusplus/upstream'),('config-utils',ROOT/'port-deps/sources/config-utils')]:
    link=sdk/name
    if not link.exists():link.symlink_to(target,target_is_directory=True)
    assert link.resolve()==target.resolve()
local_include=out/'include';local_include.mkdir(exist_ok=True)
local_curl=local_include/'libcurl';curl_source=ROOT/'port-deps/sources/libcurl-7.78.0'
if not local_curl.exists():local_curl.symlink_to(curl_source,target_is_directory=True)
assert local_curl.resolve()==curl_source.resolve()
curl_lock=json.loads((ROOT/'port-deps/sources/libcurl-7.78.0-lock.json').read_text())
assert hashlib.sha256((ROOT/'port-deps/sources/libcurl-7.78.0.a').read_bytes()).hexdigest()==curl_lock['binarySha256']
zip_source=ROOT/'port-deps/sources/zip-songdownloader/src'
includes=[local_include,zip_source,src,src/'src',src/'include',src/'shared',sdk,ROOT/'port-deps/sources/bs-cordl/include',
    il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
    ROOT/'port-deps/sources/fmt/include',ROOT/'port-deps/sources/utfcpp/source']
assert all(p.is_dir() for p in includes)
flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fvisibility=hidden','-fdeclspec',
    '-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS',
    '-DVERSION="0.8.8"','-DVERSION_LONG=0','-DMOD_ID="songdownloader"','-Wno-invalid-offsetof','-Wno-extra-qualification',
    '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [arg for path in includes for arg in ['-I',str(path)]]
adapter=ROOT/'ports/scotland2/tests/android-songdownloader-support.cpp'
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
digest.update((ROOT/'ports/scotland2/src/safe-zip.c').read_bytes())
digest.update(json.dumps(curl_lock,sort_keys=True).encode())
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
c=cc.parent/'aarch64-linux-android26-clang'
subprocess.run([str(c),*reproducible_flags(ROOT,ndk),'-O2','-fPIC','-fvisibility=hidden','-I',str(zip_source),'-c',str(ROOT/'ports/scotland2/src/safe-zip.c'),'-o',str(out/'zip.o')],check=True)
subprocess.run([str(cc),'-shared',*[str(object_path(s)) for s in sources],str(out/'zip.o'),
    str(ROOT/'port-deps/sources/libcurl-7.78.0.a'),'-lz','-llog',
    '-Lbuild/scotland2/android-aot','-lsl2_abi','-Lbuild/scotland2/android-customtypes','-lcustom-types',
    '-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2',
    '-Lbuild/scotland2/android-bsml','-lbsml','-Lbuild/scotland2/android-songcore','-lsongcore',
    '-Lbuild/scotland2/android-playlistcore','-lplaylistcore',
    '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,libsongdownloader.so','-o',str(out/'libsongdownloader.debug.so')],check=True)
subprocess.run([str(cc.parent/'llvm-objcopy'),'--strip-debug',str(out/'libsongdownloader.debug.so'),str(out/'libsongdownloader.so')],check=True)
state.write_text(stamp)
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf
elf=Elf(str(out/'libsongdownloader.so'))
assert {'setup','late_load'}<=elf.exports
if os.environ.get('SC2_BUILD_MODE')!='release':
    assert {'sc2_test_songdownloader_request','sc2_test_songdownloader_bad_tls'}<=elf.exports
imports={x for x,v in elf.imports}
assert 'A64HookFunction' not in imports
# Original OpenSSL secure-heap support imports mmap/mprotect for data pages;
# this browser installs no code hooks. Runtime executable writes remain refused.
record=json.loads((ROOT/'ports/scotland2/songdownloader/provenance.json').read_text())
record.update(buildKey=stamp,curl=curl_lock,cppSources=len(sources)-1,scope='Actual SongDownloader 0.8.8 source and original curl/zip dependencies built; no late UI or playback proof from compilation.')
record['files']={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [adapter,out/'libsongdownloader.so',ROOT/'ports/scotland2/songdownloader/upstream.patch']}
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
print('Built actual SongDownloader source:',out/'libsongdownloader.so')
