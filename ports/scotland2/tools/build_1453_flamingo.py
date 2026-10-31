#!/usr/bin/env python3
"""Build the original Flamingo 1.2.1 installer and original scene timing bridge."""
import argparse,json,subprocess,sys
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]
def build(work):
    source=work/'work-sources/flamingo';out=work/'build/flamingo';out.mkdir(exist_ok=True)
    libs=work/'build/libs';tools=work/'toolchains/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin'
    cxx=tools/'aarch64-linux-android26-clang++'
    flags=['-std=c++23','-O1','-gline-tables-only','-shared','-fPIC','-fvisibility=hidden',
           '-DSC2_AOT_HOOKS','-DANDROID','-DFLAMINGO_LOG_STANDALONE','-DNO_DEBUG_LOGS','-DNDEBUG','-DFMT_HEADER_ONLY',
           '-I'+str(source/'shared'),'-I'+str(work/'work-sources/fmt/include'),
           '-ffile-prefix-map='+str(ROOT)+'=.','-ffile-prefix-map='+str(work)+'=.',
           '-fmacro-prefix-map='+str(ROOT)+'=.','-fmacro-prefix-map='+str(work)+'=.',
           '-fdebug-compilation-dir=.','-llog','-Wl,-z,max-page-size=16384']
    run=lambda a:subprocess.run([str(x) for x in a],check=True)
    run([cxx,*flags,source/'src/installer.cpp',source/'src/capi.cpp',ROOT/'ports/scotland2/src/android-flamingo-fixups.cpp',
         '-Wl,-soname,libflamingo.so','-o',out/'libflamingo.debug.so'])
    run([tools/'llvm-objcopy','--strip-all',out/'libflamingo.debug.so',libs/'libflamingo.so'])
    run([cxx,*flags,ROOT/'ports/scotland2/src/android-flamingo-scene.cpp','-L'+str(libs),'-Wl,--no-as-needed',
         '-l:libflamingo.so','-Wl,-soname,libsc2_scene.so','-o',out/'libsc2_scene.debug.so'])
    run([tools/'llvm-objcopy','--strip-all',out/'libsc2_scene.debug.so',libs/'libsc2_scene.so'])
    sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf
    original=Elf(str(work/'elf/libflamingo.so'));ported=Elf(str(libs/'libflamingo.so'))
    missing=original.exports-ported.exports
    if missing:raise ValueError('Original Flamingo exports missing: '+repr(sorted(missing)))
    if {n for n,_ in ported.imports}&{'mprotect','mmap','__clear_cache','popen','system'}:
        raise ValueError('Flamingo still requires runtime code writing')
    receipt={'version':'1.2.1','originalExportsPreserved':len(original.exports),
             'outputs':{name:bootstrap.digest(libs/name) for name in ['libflamingo.so','libsc2_scene.so']},
             'scope':'Original installer/priority semantics and scene bridge compiled; prepared entry/runtime validation remains separate'}
    (out/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Built original Flamingo and scene bridge with patched fmt/NDK29; no executable-write imports',flush=True)
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    a=p.parse_args();build(a.workspace.resolve())
