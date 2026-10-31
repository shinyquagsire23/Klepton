#!/usr/bin/env python3
"""Initialize owned 1.45.3 IL2CPP with isolated Mac userdata, without Unity startup."""
import argparse,hashlib,json,os,subprocess
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]
def check(work,imports):
    validated=json.loads((imports/'validation.json').read_text())
    if validated['returnCode'] or 'HOST_IMPORTS_AND_CONSTRUCTORS_OK ' not in (imports/'run.log').read_text():
        raise ValueError('Require a completed import/constructor check')
    for module in validated['modules']:
        if bootstrap.digest(work/'build/libs'/module['name'])!=module['sha256']:
            raise ValueError('Module changed after import validation: '+module['name'])
    subprocess.run(['make','SC2_BUILD_MODE=release','build/t_scotland2_il2cpp'],cwd=ROOT,check=True)
    for attempt in range(100):
        folder=work/'build'/('mac-init-check'+('' if not attempt else '-'+str(attempt+1)))
        if not folder.exists():break
    else:raise ValueError('Too many retained game checks')
    folder.mkdir()
    for name in ['Frameworks','Scotland2Metadata']:
        subprocess.run(['cp','-cR',str(imports/name),str(folder/name)],check=True)
    game=ROOT/'beatsaber140';owned=json.loads((ROOT/'ports/scotland2/port1453/inputs.json').read_text())['game']
    cached_receipt=json.loads((work/'build/game-host-unprepared/translation-receipt.json').read_text())
    cached_rows={r['library']:r for r in cached_receipt['libraries']}
    translations=[]
    for name in ['libil2cpp','libunity','libmain']:
        raw=game/'lib/arm64-v8a'/(name+'.so');cached=work/'build/game-host-unprepared'/(name+'.dylib')
        if bootstrap.digest(raw)!=cached_rows[name]['elfSha256'] or bootstrap.digest(cached)!=cached_rows[name]['unpreparedTranslationSha256']:
            raise ValueError('Cached translation does not match its original game input: '+name)
        if name+'.so' in owned['files'] and bootstrap.digest(raw)!=owned['files'][name+'.so']:
            raise ValueError('Unsupported game image')
        translated=folder/'Frameworks'/(name+'.dylib')
        subprocess.run(['cp','-c',str(cached),str(translated)],check=True)
        subprocess.run(['codesign','--force','--sign','-',str(translated)],check=True)
        translations.append({'name':name,'elfSha256':bootstrap.digest(raw),'translationSha256':bootstrap.digest(translated)})
    metadata=game/'assets/bin/Data/Managed/Metadata/global-metadata.dat'
    if bootstrap.digest(metadata)!=owned['files']['global-metadata.dat']:raise ValueError('Unsupported metadata')
    userdata=folder/'userdata';userdata.mkdir()
    env={k:v for k,v in os.environ.items() if not k.startswith(('KL_','SC2_'))}
    env.update(KL_SCOTLAND2='1',KL_SCOTLAND2_ROOT=str(folder/'Scotland2Metadata'),
               KL_DYLIB_DIR=str(folder/'Frameworks'),KL_FILES_DIR=str(userdata),KL_GUEST_JIT='0',
               KL_SCOTLAND2_ABI='1',KL_SCOTLAND2_NATIVE_GATES='1',KL_SCOTLAND2_FLAMINGO='1',
               KL_SCOTLAND2_FLAMINGO_LIBRARY='libflamingo.so',SC2_INIT_ONLY='1')
    with (folder/'run.log').open('w') as log:
        result=subprocess.run([str(ROOT/'build/t_scotland2_il2cpp'),str(game/'lib/arm64-v8a/libil2cpp.so'),
                               str(game/'assets/bin/Data/Managed')],cwd=ROOT,env=env,stdout=log,stderr=subprocess.STDOUT)
    lines=(folder/'run.log').read_text().splitlines()
    markers=[line for line in lines if line.startswith('GAME_IL2CPP_INIT_OK ')]
    valid=result.returncode==0 and len(markers)==1
    (folder/'validation.json').write_text(json.dumps({'returnCode':result.returncode,'passed':valid,
        'harnessSha256':bootstrap.digest(ROOT/'build/t_scotland2_il2cpp'),'translations':translations,
        'scope':'Actual game IL2CPP initialization only; Unity startup, custom-type registration and mod callbacks remain separate'},indent=2)+'\n')
    if not valid:raise RuntimeError('Game initialization failed; inspect '+str(folder/'run.log'))
    print(markers[0])
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--imports',type=Path,required=True);a=p.parse_args();check(a.workspace.resolve(),a.imports.resolve())
