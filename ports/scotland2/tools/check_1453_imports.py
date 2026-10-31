#!/usr/bin/env python3
"""Strictly load real guest mod libraries in isolated Mac userdata; no game boot."""
import argparse,json,os,shutil,subprocess
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]

def check(work):
    # Never execute a previous harness after a failed rebuild.
    subprocess.run(['make','SC2_BUILD_MODE=release','build/t_scotland2_il2cpp','build/klepton-ld'],cwd=ROOT,check=True)
    folder=work/'build/mac-import-check'
    for attempt in range(100):
        candidate=work/'build'/('mac-import-check'+('' if not attempt else '-'+str(attempt+1)))
        if not candidate.exists():folder=candidate;break
    else:raise ValueError('Too many retained import attempts')
    frameworks=folder/'Frameworks';frameworks.mkdir(parents=True)
    metadata=folder/'Scotland2Metadata'
    for phase in ['libs','early_mods','mods','shims']:(metadata/phase).mkdir(parents=True)
    core=frameworks/'Scotland2Core.framework';core.mkdir()
    run=lambda a,**kw:subprocess.run([str(x) for x in a],check=True,cwd=ROOT,**kw)
    run(['xcrun','clang++','-dynamiclib','-Wl,-force_load,'+str(ROOT/'build/scotland2/macosx/libscotland2-core.a'),
         '-install_name','@rpath/Scotland2Core.framework/Scotland2Core','-o',core/'Scotland2Core'])
    run(['codesign','--force','--sign','-',core/'Scotland2Core'])
    rows=[]
    for library in sorted((work/'build/libs').glob('*.so')):
        # libsl2 is an Android link reference; the actual Scotland2 loader above
        # supplies that ABI. The original libzip remains a real BeatSaver dependency.
        if library.name=='libsl2.so':continue
        if library.name in {'libsl2_abi.so','libflamingo.so','libsc2_scene.so'}:phase='shims'
        elif library.name=='libc++_shared.so':phase=None
        else:phase='libs'
        if phase:shutil.copyfile(library,metadata/phase/library.name)
        translated=frameworks/(library.stem.replace('+','x')+'.dylib')
        run([ROOT/'build/klepton-ld',library,'--platform','macos','--install-name','@rpath/'+translated.name,
             '-o',translated,'--quiet'])
        run(['codesign','--force','--sign','-',translated])
        rows.append({'name':library.name,'sha256':bootstrap.digest(library),'translationSha256':bootstrap.digest(translated)})
    inventory=folder/'expected-modules.txt'
    # The scene bridge is prepared only when installing its lifecycle hook. It
    # stays excluded here because this check intentionally executes no callbacks.
    inventory.write_text(''.join(r['name']+'\n' for r in rows if r['name']!='libsc2_scene.so'))
    userdata=folder/'userdata';userdata.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith(('KL_','SC2_'))}
    env.update(KL_SCOTLAND2='1',KL_SCOTLAND2_ROOT=str(metadata),KL_DYLIB_DIR=str(frameworks),
               KL_FILES_DIR=str(userdata),KL_GUEST_JIT='0',KL_SCOTLAND2_ABI='1',
               KL_SCOTLAND2_NATIVE_GATES='1',KL_SCOTLAND2_FLAMINGO='1',
               KL_SCOTLAND2_FLAMINGO_LIBRARY='libflamingo.so',SC2_IMPORTS_ONLY=str(inventory))
    with (folder/'run.log').open('w') as log:
        result=subprocess.run([str(ROOT/'build/t_scotland2_il2cpp'),str(ROOT/'beatsaber140/lib/arm64-v8a/libil2cpp.so'),
                               str(ROOT/'beatsaber140/assets/bin/Data/Managed')],env=env,cwd=ROOT,stdout=log,stderr=subprocess.STDOUT)
    receipt={'modules':rows,'returnCode':result.returncode,'harnessSha256':bootstrap.digest(ROOT/'build/t_scotland2_il2cpp'),
             'translatorSha256':bootstrap.digest(ROOT/'build/klepton-ld'),
             'scope':'Actual signed guest library constructors and strict imports only; game initialization, mod setup/load/late callbacks and gameplay are not exercised'}
    (folder/'validation.json').write_text(json.dumps(receipt,indent=2)+'\n')
    if result.returncode:raise RuntimeError('Strict load failed; inspect '+str(folder/'run.log'))
    lines=(folder/'run.log').read_text().splitlines()
    markers=[line for line in lines if line.startswith('HOST_IMPORTS_AND_CONSTRUCTORS_OK ')]
    if len(markers)!=1:raise RuntimeError('Import-only completion marker missing; not a passed check')
    print(markers[0])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    a=p.parse_args();check(a.workspace.resolve())
