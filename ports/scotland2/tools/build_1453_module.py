#!/usr/bin/env python3
"""Build pinned Unity 6.3 mod sources in an explicitly prepared private workspace.

This development driver does not package, sign, install or claim game support.
Source reconstruction and shipping selection are validated separately.
"""
import argparse
import concurrent.futures
import functools
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import shlex
import struct
import subprocess
import sys
import time

ROOT = Path(__file__).resolve().parents[3]
MODULES = {
    'bshook': ('9.3.0', 'beatsaber-hook', 'libbeatsaber-hook'),
    'customtypes': ('0.21.2', 'custom-types', 'libcustom-types'),
    'lapiz': ('0.3.7', 'lapiz', 'liblapiz'),
    'bsml': ('0.6.5', 'bsml', 'libbsml'),
    'metacore': ('2.0.6', 'metacore', 'libmetacore'),
    'metacore-bs': ('1.0.1', 'metacore-bs', 'libmetacore-bs'),
    'songcore': ('1.3.5', 'SongCore', 'libsongcore'),
    'playlistcore': ('1.7.2', 'playlistcore', 'libplaylistcore'),
    'songdownloader': ('0.9.1', 'songdownloader', 'libsongdownloader'),
    'unicode': ('0.1.8', 'unicode', 'libunicode'),
    'datakeeper': ('0.2.7', 'datakeeper', 'libdatakeeper'),
    'bettersongsearch': ('2.3.0', 'BetterSongSearch', 'libBetterSongSearch'),
    'airbuds-search': ('2.0.2', 'airbuds-search', 'libairbuds-search'),
    'web-utils': ('0.6.9', 'WebUtils', 'libweb-utils'),
    'song-details': ('1.0.5', 'song-details', 'libsongdetails'),
    'beatsaverplusplus': ('0.2.4', 'beatsaverplusplus', 'libbeatsaverplusplus'),
}

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as file:
        for chunk in iter(lambda: file.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def validate_unity_profile(profile_path, unity_path):
    sys.path.insert(0, str(ROOT/'tools'))
    from audit_elf import Elf
    profile = json.loads(profile_path.read_text())
    image = Elf(str(unity_path))
    if hashlib.sha256(image.data).hexdigest() != profile['unitySha256']:
        raise ValueError('MetaCore Unity profile hash mismatch')
    def word(address):
        if not any(s[1] == 1 and s[2] & 4 and s[3] <= address <= s[3]+s[5]-4 for s in image.sh):
            raise ValueError('MetaCore profile points outside executable code')
        return struct.unpack_from('<I', image.data, image._off(address))[0]
    target = int(profile['target']['vaddr'], 0)
    anchors = profile['bodyWords'] + [profile['target']]
    anchors += [dict(vaddr=hex(target+i*4), expect=w) for i,w in enumerate(profile['entryWords'])]
    for anchor in anchors:
        if word(int(anchor['vaddr'],0)) != int(anchor['expect'],0):
            raise ValueError('MetaCore Unity instruction mismatch')
    for call in profile['callsites']:
        pc = int(call['vaddr'],0); instruction = word(pc)
        delta = instruction & 0x3ffffff
        if delta & 1<<25: delta -= 1<<26
        if instruction != int(call['expect'],0) or instruction & 0x7c000000 != 0x14000000 or pc+delta*4 != target:
            raise ValueError('MetaCore Unity cleanup callsite mismatch')
    return profile

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('module', choices=MODULES)
    parser.add_argument('--workspace', type=Path, required=True)
    parser.add_argument('--legacy-dependencies', type=Path, required=True)
    parser.add_argument('--jobs', type=int, default=2)
    parser.add_argument('--sdk-tests', action='store_true', help='Also compile the upstream bshook hook fixture; does not run it')
    parser.add_argument('--sdk-tests-only', action='store_true', help='Compile the bshook fixture without relinking or changing runtime libraries')
    args = parser.parse_args()
    work, legacy = args.workspace.resolve(), args.legacy_dependencies.resolve()
    source = work / 'work-sources' / args.module
    sdk = work / 'build/sdk'
    ndk = (work / 'toolchains/android-ndk-r29').resolve()
    ndk_receipt = json.loads((ndk / 'SC2-NDK-RECEIPT.json').read_text())
    if ndk_receipt['version'] != '29.0.14206865':
        raise ValueError('Wrong compiler profile')
    tools = ndk / 'toolchains/llvm/prebuilt/darwin-x86_64/bin'
    cxx = tools / 'aarch64-linux-android26-clang++'
    out = work / 'build' / args.module
    objects = out / 'objects'
    objects.mkdir(parents=True, exist_ok=True)
    (out / 'include').mkdir(exist_ok=True)
    (out/'include/git_info.h').write_text('#pragma once\n// Pinned build identity is supplied in compiler definitions.\n')
    libs = work / 'build/libs'
    version, mod_id, soname = MODULES[args.module]
    if args.module in {'song-details','songdownloader'}:
        local_curl = out/'include/libcurl'
        if not local_curl.exists(): local_curl.symlink_to(legacy/'libcurl-7.78.0',target_is_directory=True)
        if local_curl.resolve() != (legacy/'libcurl-7.78.0').resolve():
            raise ValueError('Wrong curl headers for the original module')
    if args.module == 'airbuds-search':
        declarations = ['#pragma once', '#include "metacore/shared/assets.hpp"',
                        'namespace IncludedAssets { namespace Externs {']
        assets = sorted(p for p in (source/'assets').rglob('*') if p.is_file())
        for asset in assets:
            relative = str(asset.relative_to(source/'assets'))
            if '/' in relative: raise ValueError('Nested Airbuds assets need namespace review')
            symbol = '_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',relative)
            declarations += ['extern "C" uint8_t '+symbol+'_start[];', 'extern "C" uint8_t '+symbol+'_end[];']
        declarations.append('}')
        for asset in assets:
            name = re.sub(r'[^a-zA-Z0-9_]','_',asset.name); symbol = '_binary_'+name
            declarations.append('inline const IncludedAsset '+name+'{Externs::'+symbol+'_start,Externs::'+symbol+'_end};')
        declarations.append('}')
        (out/'include/assets.hpp').write_text('\n'.join(declarations)+'\n')
    il2cpp = work / 'sources/libil2cpp/il2cpp'
    includes = [out/'include', source, source/'include', source/'shared', source/'src', sdk,
                ROOT/'ports/scotland2/src', ROOT/'ports/scotland2/include',
                work/'sources/bs-cordl/include', il2cpp/'libil2cpp',
                il2cpp/'external/baselib/Include', il2cpp/'external/baselib/Platforms/Android/Include',
                work/'work-sources/fmt/include', legacy/'utfcpp/source', work/'sources/reflectcpp/include',
                legacy/'css-color-parser-cpp', legacy/'EasyGifReader', legacy/'cppcodec',
                legacy/'gif-lib/shared', legacy/'gif-lib/include', legacy/'gif-lib/src']
    if args.module == 'song-details':
        includes += [legacy/'protobuf/src', legacy/'brotli/c/include']
    if args.module == 'songdownloader':
        includes += [legacy/'zip-songdownloader/src']
    cmake = (source/'CMakeLists.txt').read_text()
    standard = re.search(r'set\(CMAKE_CXX_STANDARD\s+(\d+)\)', cmake)
    language = '-std=c++2c' if standard and int(standard[1]) >= 26 else '-std=c++23'
    flags = [language, '-O1', '-gline-tables-only', '-fPIC', '-frtti', '-fexceptions',
             '-fvisibility=hidden', '-fdeclspec', '-DANDROID', '-DUNITY_6', '-DUNITY_6_3',
             '-DHAS_CODEGEN', '-DNEED_UNSAFE_CSHARP', '-DFMT_HEADER_ONLY', '-DSC2_AOT_HOOKS',
             '-DSC2_DIAGNOSTICS=0', '-DNDEBUG', '-DREFLECT_CPP_C_ARRAYS_OR_INHERITANCE',
             '-DVERSION="'+version+'"', '-DMOD_ID="'+mod_id+'"', '-DVERSION_NUMBER=300150000',
             '-DVERSION_LONG=0', '-DGIT_COMMIT=0', '-DGIT_MODIFIED=0', '-DGIT_BRANCH="unity63-port"',
             '-DGIT_USER="source-port"', '-DHOT_RELOAD=0', '-DMAKE_DOCS=0',
             '-Wno-invalid offsetof'.replace(' ', '-'), '-Wno-extra-qualification', '-Wno-vla-cxx-extension',
             '-include', str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp'),
             '-include', str(ROOT/'ports/scotland2/port1453/sc2-hook-catalog.hpp'),
             '-ffile-prefix-map='+str(work)+'=.', '-fmacro-prefix-map='+str(work)+'=.',
             '-ffile-prefix-map='+str(ROOT)+'=.', '-fmacro-prefix-map='+str(ROOT)+'=.',
             '-ffile-prefix-map='+str(legacy)+'=/dependencies', '-fmacro-prefix-map='+str(legacy)+'=/dependencies',
             '-ffile-prefix-map='+str(ndk)+'=/ndk', '-fmacro-prefix-map='+str(ndk)+'=/ndk',
             '-fdebug-compilation-dir=.']
    flags += [value for path in includes if path.is_dir() for value in ['-I', str(path)]]
    if args.module in {'bshook','playlistcore'}:
        # These upstream libraries export their API through default visibility;
        # hiding it drops the i2c and public PlaylistCore ABI used by other mods.
        flags.append('-fvisibility=default')
    if args.module == 'unicode':
        flags.append('-DSC2_UNICODE_APPLE_GLYPHS')
    if args.module == 'songcore':
        # Required by the original SHA-1 NEON implementation and upstream build.
        flags.append('-march=armv8-a+crypto')
    if args.module == 'web-utils':
        flags += ['-DWEBUTILS_NO_JSON', '-DWEBUTILS_NO_BSML']
    if args.module in {'song-details','beatsaverplusplus'}: flags += ['-DWEBUTILS_NO_BSML']
    if args.module == 'customtypes':
        from find_customtypes_targets import profile_unity63
        game = ROOT/'beatsaber140/lib/arm64-v8a'
        expected = json.loads((ROOT/'ports/scotland2/port1453/inputs.json').read_text())['game']['files']
        for name in ['libil2cpp.so','libunity.so']:
            if digest(game/name) != expected[name]: raise ValueError('Unsupported custom-types game input: '+name)
        targets = profile_unity63(game/'libil2cpp.so',game/'libunity.so')
        (work/'customtypes-targets.json').write_text(json.dumps(targets,indent=2)+'\n')
        target, = targets['unity']['targets']
        header = out/'include/sc2-customtypes-target.h'
        header.write_text('#define SC2_CUSTOMTYPES_SCRIPTING_CLASS_OFFSET '+target['vaddr']+'\n'
                          '#define SC2_CUSTOMTYPES_SCRIPTING_CLASS_WORD '+target['expect']+'u\n')
        flags += ['-include', str(header)]
    if args.module == 'metacore':
        profile = validate_unity_profile(ROOT/'ports/scotland2/port1453/metacore-unity-targets.json',
                                        ROOT/'beatsaber140/lib/arm64-v8a/libunity.so')
        flags += ['-DSC2_METACORE_DELETE_OFFSET='+profile['target']['vaddr']]
    sources = sorted((source/'src').glob('*.cpp') if args.module == 'bshook' else (source/'src').rglob('*.cpp'))
    # Lapiz's src/test is a separate runtime mod with its own setup entrypoint.
    # Header-only custom-types test files remain guarded by LOCAL_TEST upstream.
    sources = [p for p in sources if not {'test', 'tests'} & set(p.relative_to(source/'src').parts[:-1])]
    if args.module == 'song-details': sources += sorted((source/'src').rglob('*.cc'))
    if args.module == 'customtypes':
        sources.append(ROOT/'ports/scotland2/src/android-customtypes-target.cpp')
    if args.module == 'bsml':
        sources += [legacy/'css-color-parser-cpp/csscolorparser.cpp', legacy/'EasyGifReader/EasyGifReader.cpp']
    if args.module == 'airbuds-search':
        sources = [p for p in sources if p.name != 'Encryption.cpp']
    if not sources:
        raise ValueError('Original sources are missing')
    # Track actual compiler dependencies. Editing Airbuds must not invalidate
    # every unrelated BSML translation unit; APFS clone mtimes are not proof.
    stamp = hashlib.sha256(json.dumps(flags).encode()).hexdigest()
    @functools.lru_cache(maxsize=None)
    def cached_digest(path, modified, size): return digest(Path(path))
    def dependency_digest(path):
        status = Path(path).stat()
        return cached_digest(path,status.st_mtime_ns,status.st_size)
    def object_key(path, dependencies):
        key = hashlib.sha256(stamp.encode()+path.read_bytes())
        for dependency in dependencies:
            key.update(dependency.encode()); key.update(dependency_digest(dependency).encode())
        return key.hexdigest()
    def object_name(path):
        return hashlib.sha256(str(path).encode()).hexdigest()[:16]+'-'+path.name+'.o'
    def compile_one(path):
        name = object_name(path)
        obj = objects/name
        receipt = obj.with_suffix('.key')
        dependency_file = obj.with_suffix('.deps.json')
        dependencies = json.loads(dependency_file.read_text()) if dependency_file.exists() else None
        fresh = False
        if dependencies and obj.exists() and receipt.exists():
            try: fresh = receipt.read_text() == object_key(path,dependencies)
            except OSError: pass
        if not fresh:
            print('Compiling', args.module, path.name, flush=True)
            started = time.time_ns()
            with obj.with_suffix('.log').open('w') as log:
                result = subprocess.run([str(cxx), *flags, '-MMD', '-MF', str(obj.with_suffix('.d')),
                                         '-c', str(path), '-o', str(obj)], stdout=log, stderr=subprocess.STDOUT)
            if result.returncode:
                raise RuntimeError(obj.with_suffix('.log').read_text())
            dependency_text = obj.with_suffix('.d').read_text().replace('\\\n','')
            dependencies = sorted(set(shlex.split(dependency_text.split(':',1)[1])))
            if any(Path(p).stat().st_mtime_ns > started for p in dependencies):
                receipt.unlink(missing_ok=True)
                raise RuntimeError('Source changed during compilation; rerun after edits finish: '+path.name)
            dependency_file.write_text(json.dumps(dependencies))
            receipt.write_text(object_key(path,dependencies))
        return obj
    if args.sdk_tests_only:
        if args.module != 'bshook': raise ValueError('SDK fixture belongs to bshook')
        compile_one(source/'src/tests/hooking.cpp')
        print('Compiled SDK fixture only; no runtime library changed and no test executed',flush=True)
        return
    # Stop scheduling work after a compiler failure, while retaining valid objects.
    compiled, pending = [], set()
    remaining = iter(sources)
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1,min(4,args.jobs))) as pool:
        for _ in range(max(1,min(4,args.jobs))):
            path = next(remaining, None)
            if path: pending.add(pool.submit(compile_one, path))
        while pending:
            done, pending = concurrent.futures.wait(pending, return_when=concurrent.futures.FIRST_COMPLETED)
            for future in done:
                compiled.append(future.result())
            for _ in done:
                path = next(remaining, None)
                if path: pending.add(pool.submit(compile_one, path))
    if args.sdk_tests:
        if args.module != 'bshook': raise ValueError('SDK fixture belongs to bshook')
        compile_one(source/'src/tests/hooking.cpp')
    # Existing generated IncludedAsset headers use the original 32-byte array
    # prefix. New #embed headers need no generated assembly.
    asset_header = (out/'include/assets.hpp') if args.module == 'airbuds-search' else source/'include/assets.hpp'
    if asset_header.exists() and '_binary_' in asset_header.read_text():
        assembly = ['.section .data.sc2_assets,"aw",@progbits']
        for asset in sorted((source/'assets').rglob('*')):
            if not asset.is_file(): continue
            symbol = '_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',str(asset.relative_to(source/'assets')))
            assembly += ['.balign 8','.global '+symbol+'_start','.global '+symbol+'_end',
                         symbol+'_start:','.space 32,32','.incbin '+json.dumps(str(asset)),'.byte 32',symbol+'_end:']
        path = out/'assets.S'; path.write_text('\n'.join(assembly)+'\n')
        subprocess.run([str(tools/'aarch64-linux-android26-clang'),'-c',str(path),'-o',str(out/'assets.o')],check=True)
        compiled.append(out/'assets.o')
    if args.module == 'bsml':
        for source_file in sorted((legacy/'gif-lib/src').glob('*.c')):
            obj = objects/('gif-'+source_file.stem+'.o')
            subprocess.run([str(tools/'aarch64-linux-android26-clang'),'-O1','-fPIC','-fvisibility=hidden',
                            '-I'+str(legacy/'gif-lib/shared'),'-I'+str(legacy/'gif-lib/include'),'-I'+str(legacy/'gif-lib/src'),
                            '-c',str(source_file),'-o',str(obj)],check=True)
            compiled.append(obj)
    # Match the declared original dynamic dependency set, adding only the
    # prepared platform support DSO. Their shipping inventory is a later check.
    sys.path.insert(0,str(ROOT/'tools'))
    from audit_elf import Elf
    if args.module == 'airbuds-search':
        # Airbuds has no Unity 6.3 upstream binary. Link its original source
        # against the explicitly selected modern dependency ABI.
        dependencies = ['libbeatsaber-hook.so','libcustom-types.so','libbsml.so','libsongcore.so',
                        'libbeatsaverplusplus.so','libweb-utils.so','libsongdetails.so','libmetacore.so',
                        'libmetacore-bs.so','libpaper2_scotland2.so','libflamingo.so','libreflectcpp.so']
        if not all((libs/n).is_file() for n in dependencies): raise ValueError('Airbuds dependency missing')
    else:
        original = Elf(str(work/'elf'/(soname+'.so')))
        dependencies = [n for n in original.needed if (libs/n).is_file() and n!=soname+'.so']
    # Cache filenames contain private workspace paths only through a hash. They
    # must never determine link order: that changed static initialization order
    # and output bytes when the same sources were built in another folder.
    source_order = {object_name(path):i for i,path in enumerate(sources)}
    compiled.sort(key=lambda p:(source_order.get(p.name,len(sources)),p.name))
    command = [str(cxx),'-shared',*[str(p) for p in compiled],'-L'+str(libs)]
    command += ['-l:'+n for n in dependencies]
    if args.module in {'song-details','songdownloader'}:
        curl = legacy/'libcurl-7.78.0.a'
        expected = json.loads((ROOT/'port-deps/sources/libcurl-7.78.0-lock.json').read_text())['binarySha256']
        if digest(curl) != expected: raise ValueError('Pinned curl 7.78 archive mismatch')
        command.append(str(curl))
    if args.module == 'song-details':
        from build_1453_network import build
        command += [str(p) for p in build(work,legacy,args.jobs)]
    if args.module == 'songdownloader':
        zip_object = out/'safe-zip.o'
        subprocess.run([str(tools/'aarch64-linux-android26-clang'),'-O1','-fPIC','-fvisibility=hidden',
                        '-I'+str(legacy/'zip-songdownloader/src'),'-ffile-prefix-map='+str(ROOT)+'=.',
                        # miniz's asserts name their header: without these the build machine's path ships in the library.
                        '-ffile-prefix-map='+str(legacy)+'=/dependencies','-fmacro-prefix-map='+str(legacy)+'=/dependencies',
                        '-c',str(ROOT/'ports/scotland2/src/safe-zip.c'),'-o',str(zip_object)],check=True)
        command.append(str(zip_object))
    if args.module == 'web-utils':
        curl = legacy/'libcurl-8.5.0.a'
        expected = json.loads((ROOT/'port-deps/sources/libcurl-8.5.0-lock.json').read_text())['binarySha256']
        if digest(curl) != expected: raise ValueError('Pinned curl 8.5.0 archive mismatch')
        command.append(str(curl))
    command += ['-Wl,--no-as-needed','-l:libsl2_abi.so','-llog','-lz','-ldl','-static-libstdc++',
                '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,'+soname+'.so',
                '-o',str(out/(soname+'.debug.so'))]
    subprocess.run(command,check=True)
    subprocess.run([str(tools/'llvm-objcopy'),'--strip-all',str(out/(soname+'.debug.so')),str(libs/(soname+'.so'))],check=True)
    elf = Elf(str(libs/(soname+'.so')))
    imports = {n for n,_ in elf.imports}
    forbidden = {'mprotect','popen','system'}
    if args.module in {'web-utils','song-details','songdownloader'}:
        # The verified curl archive's mem_sec.o uses mprotect for OpenSSL data
        # guard pages. The host klb_mprotect rejects all executable requests.
        forbidden.remove('mprotect')
    if imports & forbidden:
        raise ValueError('Module still imports a runtime code/shell interface: '+str(imports & forbidden))
    input_key = hashlib.sha256(stamp.encode())
    for obj in sorted(compiled):
        key_file = obj.with_suffix('.key')
        input_key.update((key_file.read_text() if key_file.exists() else digest(obj)).encode())
    record = {'module':args.module,'version':version,'outputSha256':digest(libs/(soname+'.so')),
              'sourceFiles':len(sources),'needed':elf.needed,'buildKey':input_key.hexdigest(),
              'status':'Compiled original source for Android arm64; not packaged, installed or gameplay-validated'}
    (out/'build-receipt.json').write_text(json.dumps(record,indent=2)+'\n')
    print('Built',soname,version,flush=True)

if __name__ == '__main__':
    try: main()
    except (ValueError,OSError,RuntimeError,subprocess.CalledProcessError) as error:
        raise SystemExit(str(error))
