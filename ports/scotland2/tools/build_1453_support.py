#!/usr/bin/env python3
"""Build the existing Android C++/Capstone/ZIP bridges with the pinned NDK29."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import bootstrap

ROOT=Path(__file__).resolve().parents[3]
def build(work,legacy):
    out=work/'build/support';out.mkdir(parents=True,exist_ok=True)
    libs=work/'build/libs';libs.mkdir(exist_ok=True)
    sdk=work/'build/sdk';ndk=work/'toolchains/android-ndk-r29'
    receipt=json.loads((ndk/'SC2-NDK-RECEIPT.json').read_text())
    if receipt['version']!='29.0.14206865': raise ValueError('Wrong compiler profile')
    tools=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin'
    cxx=tools/'aarch64-linux-android26-clang++';cc=tools/'aarch64-linux-android26-clang'
    capstone=legacy/'capstone-qpm/libcapstone.a'
    if bootstrap.digest(capstone)!=json.loads((ROOT/'port-deps/sources/capstone-qpm/lock.json').read_text())['sha256']:
        raise ValueError('Capstone archive mismatch')
    run=lambda args:subprocess.run([str(a) for a in args],check=True)
    run([tools/'llvm-objcopy','--redefine-sym','cs_disasm_iter=sc2_capstone_iter_raw',
         '--redefine-sym','cs_disasm=sc2_capstone_disasm_raw',capstone,out/'libcapstone-view.a'])
    maps=['-ffile-prefix-map='+str(ROOT)+'=.','-ffile-prefix-map='+str(work)+'=.',
          '-ffile-prefix-map='+str(legacy)+'=/dependencies','-ffile-prefix-map='+str(ndk)+'=/ndk',
          '-fmacro-prefix-map='+str(ROOT)+'=.','-fmacro-prefix-map='+str(work)+'=.',
          '-fdebug-compilation-dir=.']
    run([cc,*maps,'-O1','-fPIC','-fvisibility=hidden','-I'+str(legacy/'zip-songdownloader/src'),
         '-c',ROOT/'ports/scotland2/src/safe-zip.c','-o',out/'safe-zip.o'])
    exports=out/'exports.map'
    exports.write_text('{ global: _ZN9modloader*; cs_*; A64HookFunction; A64HookFunctionV; sc2_bshook_mkpath; sc2_beatsaver_extract; local: *; };\n')
    sources=[ROOT/'ports/scotland2/src/android-paths.cpp',ROOT/'ports/scotland2/and64/shared/inline-hook/And64InlineHook.cpp',
             ROOT/'ports/scotland2/src/android-bshook-filesystem.cpp',ROOT/'ports/scotland2/src/android-beatsaver-extract.cpp',
             ROOT/'ports/scotland2/src/android-capstone-view.cpp',out/'safe-zip.o']
    run([cxx,*maps,'-std=c++23','-O1','-gline-tables-only','-fPIC','-shared','-fvisibility=hidden',
         '-DSL2_KLEPTON','-DSC2_AOT_HOOKS','-I'+str(ROOT/'ports/scotland2/upstream/shared'),'-I'+str(sdk),
         *sources,'-Wl,--version-script,'+str(exports),'-Wl,--whole-archive',out/'libcapstone-view.a',
         '-Wl,--no-whole-archive','-Wl,-z,max-page-size=16384','-Wl,-soname,libsl2_abi.so','-o',out/'libsl2_abi.debug.so'])
    run([tools/'llvm-objcopy','--strip-all',out/'libsl2_abi.debug.so',libs/'libsl2_abi.so'])
    shutil.copyfile(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so',libs/'libc++_shared.so')
    # Original ELFs are strictly link references until a selected module replaces
    # them. Packaging must use an explicit composition, never this whole folder.
    for original in (work/'elf').glob('*.so'):
        target=libs/original.name
        if not target.exists(): shutil.copyfile(original,target)
    (out/'build-receipt.json').write_text(json.dumps({'capstoneSha256':bootstrap.digest(capstone),
        'outputSha256':bootstrap.digest(libs/'libsl2_abi.so'),'scope':'Original ABI/ZIP/Capstone adapters compiled; no runtime load or application packaging performed'},indent=2)+'\n')
    print('Built original Android support adapters with NDK29',flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--legacy-dependencies',type=Path,required=True);a=p.parse_args()
    build(a.workspace.resolve(),a.legacy_dependencies.resolve())
