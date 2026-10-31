#!/usr/bin/env python3
"""Exercise actual SDK/custom types against prepared owned game images on Mac."""
import argparse,json,os,subprocess
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]
def check(work,imports,legacy,early=False):
    validated=json.loads((imports/'validation.json').read_text())
    if validated['returnCode'] or 'HOST_IMPORTS_AND_CONSTRUCTORS_OK ' not in (imports/'run.log').read_text():
        raise ValueError('Require a completed import/constructor check')
    for module in validated['modules']:
        if bootstrap.digest(work/'build/libs'/module['name'])!=module['sha256']:
            raise ValueError('Module changed after import validation: '+module['name'])
    run=lambda a,**kw:subprocess.run([str(x) for x in a],check=True,cwd=ROOT,**kw)
    run(['make','SC2_BUILD_MODE=release','build/t_scotland2_il2cpp'])
    for attempt in range(100):
        folder=work/'build'/(('mac-early-check' if early else 'mac-registration-check')+('' if not attempt else '-'+str(attempt+1)))
        if not folder.exists():break
    else:raise ValueError('Too many retained registration checks')
    folder.mkdir()
    for name in ['Frameworks','Scotland2Metadata']:
        run(['cp','-cR',imports/name,folder/name])
    if early:
        manifest=json.loads((ROOT/'ports/scotland2/port1453/inputs.json').read_text())
        for package in manifest['packages']:
            for field,phase in [('earlyModFiles','early_mods'),('lateModFiles','mods')]:
                for name in package[field]:
                    (folder/'Scotland2Metadata/libs'/name).rename(folder/'Scotland2Metadata'/phase/name)
        (folder/'Scotland2Metadata/libs/libairbuds-search.so').rename(folder/'Scotland2Metadata/mods/libairbuds-search.so')
    tools=work/'toolchains/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin'
    il2cpp=work/'sources/libil2cpp/il2cpp'
    includes=[work/'build/sdk',work/'sources/bs-cordl/include',il2cpp/'libil2cpp',
              il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include',
              work/'work-sources/fmt/include',legacy/'utfcpp/source']
    fixture=folder/'Scotland2Metadata/shims/libsc2_1453_registration.so'
    run([tools/'aarch64-linux-android26-clang++','-shared','-std=c++23','-O1','-gline-tables-only',
         '-fPIC','-frtti','-fexceptions','-fvisibility=hidden','-fdeclspec','-DANDROID','-DUNITY_6','-DUNITY_6_3',
         '-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS','-DSC2_DIAGNOSTICS=0',
         '-include',ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp',
         *[v for p in includes for v in ['-I',p]],ROOT/'ports/scotland2/tests/android-1453-registration.cpp',
         '-L'+str(work/'build/libs'),'-l:libcustom-types.so','-l:libbeatsaber-hook.so','-l:libpaper2_scotland2.so',
         '-l:libflamingo.so','-l:libsl2_abi.so','-llog','-ldl','-lz','-static-libstdc++',
         '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,'+fixture.name,'-o',fixture])
    translated=folder/'Frameworks/libsc2_1453_registration.dylib'
    run([ROOT/'build/klepton-ld',fixture,'--platform','macos','--install-name','@rpath/'+translated.name,'-o',translated,'--quiet'])
    run(['codesign','--force','--sign','-',translated])
    prepared=work/'build/game-host-prepared'
    receipt=json.loads((prepared/'preparation-receipt.json').read_text())
    game=ROOT/'beatsaber140';translations=[]
    main=next(r for r in json.loads((work/'build/game-host-unprepared/translation-receipt.json').read_text())['libraries'] if r['library']=='libmain')
    main_source=work/'build/game-host-unprepared/libmain.dylib'
    if bootstrap.digest(game/'lib/arm64-v8a/libmain.so')!=main['elfSha256'] or bootstrap.digest(main_source)!=main['unpreparedTranslationSha256']:
        raise ValueError('Main game translation changed')
    run(['cp','-c',main_source,folder/'Frameworks/libmain.dylib'])
    run(['codesign','--force','--sign','-',folder/'Frameworks/libmain.dylib'])
    for row in receipt['libraries']:
        name=row['library'];raw=game/'lib/arm64-v8a'/(name+'.so');source=prepared/(name+'.dylib')
        if bootstrap.digest(raw)!=row['elfSha256'] or bootstrap.digest(source)!=row['preparedTranslationSha256']:
            raise ValueError('Game preparation changed: '+name)
        target=folder/'Frameworks'/(name+'.dylib');run(['cp','-c',source,target]);run(['codesign','--force','--sign','-',target])
        translations.append(dict(row,signedSha256=bootstrap.digest(target)))
    # The measured SDK scan order is a separate input to the guest's results.
    targets=json.loads((work/'customtypes-targets.json').read_text())['il2cpp']['targets']
    sdk_profile=folder/'sdk-profile.txt';ct_profile=folder/'customtypes-profile.txt'
    sdk_profile.write_text(''.join(t['vaddr']+' '+t['name']+'\n' for t in targets[:3]))
    ct_profile.write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name']+'\n' for t in targets))
    userdata=folder/'userdata';userdata.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith(('KL_','SC2_'))}
    env.update(KL_SCOTLAND2='1',KL_SCOTLAND2_ROOT=str(folder/'Scotland2Metadata'),
               KL_DYLIB_DIR=str(folder/'Frameworks'),KL_FILES_DIR=str(userdata),KL_GUEST_JIT='0',
               KL_SCOTLAND2_ABI='1',KL_SCOTLAND2_NATIVE_GATES='1',KL_SCOTLAND2_FLAMINGO='1',
               KL_SCOTLAND2_FLAMINGO_LIBRARY='libflamingo.so',SC2_1453_PROBE=str(fixture),
               SC2_1453_UNITY=str(game/'lib/arm64-v8a/libunity.so'),SC2_1453_SDK_PROFILE=str(sdk_profile),
               SC2_1453_CT_PROFILE=str(ct_profile))
    if early:
        env.update(SC2_1453_EARLY='1',KL_SCOTLAND2_FLAMINGO_PROBE='1',KL_SCOTLAND2_SCENE_HOOK='1')
        for name,count in [('lapiz',26),('bsml',14)]:
            catalog=json.loads((work/'build/hooks-1453'/(name+'-catalog.json')).read_text())
            filename='lib'+name+'.so'
            if bootstrap.digest(work/'build/libs'/filename)!=catalog['modSha256'] or len(catalog['targets'])!=count:
                raise ValueError('Stale early-mod hook catalog: '+name)
            path=folder/(name+'-hooks.txt')
            path.write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name']+'\n' for t in catalog['targets']))
            env['SC2_1453_'+name.upper()+'_PROFILE']=str(path)
        scene_profile=json.loads((work/'build/hooks-1453/scene-target.json').read_text())
        scene=scene_profile['target']
        path=folder/'scene-hook.txt';path.write_text(scene['vaddr']+' '+scene['expect']+' scene\n')
        env['SC2_1453_SCENE_PROFILE']=str(path)
        path=folder/'host-scene-icall.txt';path.write_text(scene_profile['destroyImmediate']+' '+scene_profile['icall']+'\n')
        env['SC2_1453_SCENE_ICALL']=str(path)
    with (folder/'run.log').open('w') as log:
        result=subprocess.run([str(ROOT/'build/t_scotland2_il2cpp'),str(game/'lib/arm64-v8a/libil2cpp.so'),
                               str(game/'assets/bin/Data/Managed')],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
    markers=[line for line in (folder/'run.log').read_text().splitlines() if line.startswith('SDK_REGISTRATION_1453_OK ')]
    valid=result.returncode==0 and len(markers)==1
    if early:valid=valid and (folder/'run.log').read_text().count('EARLY_MODS_1453_OK ')==1
    (folder/'validation.json').write_text(json.dumps({'returnCode':result.returncode,'passed':valid,
        'harnessSha256':bootstrap.digest(ROOT/'build/t_scotland2_il2cpp'),'translations':translations,
        'fixtureSha256':bootstrap.digest(fixture),'modules':validated['modules'],
        'scope':('Original early mod setup/load, SDK/custom types and scene hook with measured host icall registration; no production Unity icall registration, Unity startup, late callbacks or scene execution' if early else
                 'Actual SDK initialization/custom-type registration and benign object construction; no Unity startup or mod load callbacks')},indent=2)+'\n')
    if not valid:raise RuntimeError('Registration failed; inspect '+str(folder/'run.log'))
    print(markers[0])
    if early:print(next(line for line in (folder/'run.log').read_text().splitlines() if line.startswith('EARLY_MODS_1453_OK ')))
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--imports',type=Path,required=True);p.add_argument('--legacy-dependencies',type=Path,required=True)
    p.add_argument('--early',action='store_true')
    a=p.parse_args();check(a.workspace.resolve(),a.imports.resolve(),a.legacy_dependencies.resolve(),a.early)
