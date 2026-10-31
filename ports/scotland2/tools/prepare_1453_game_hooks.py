#!/usr/bin/env python3
"""Prepare measured game hook gates offline, before Mac signing or execution."""
import argparse,json,subprocess,sys
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]
def game_profiles(combined):
    unity={int(t['vaddr'],0):dict(t) for t in combined['customtypesUnity']['targets']}
    for target in [combined['metacoreUnity']['target'],combined['sceneUnity']['target']]:
        address=int(target['vaddr'],0)
        if address in unity and int(unity[address]['expect'],0)!=int(target['expect'],0):raise ValueError('Conflicting Unity target')
        unity[address]=target
    return {'libil2cpp':combined['il2cpp'],'libunity':{'sha256':combined['customtypesUnity']['sha256'],
                                                   'targets':[unity[a] for a in sorted(unity)],'originalEntryDepth':2}}
def prepare(work):
    combined=json.loads((work/'build/hooks-1453/combined-targets.json').read_text())
    output=work/'build/game-host-prepared';output.mkdir(exist_ok=True)
    rows=game_profiles(combined)
    receipts=[]
    originals=json.loads((work/'build/game-host-unprepared/translation-receipt.json').read_text())
    originals={r['library']:r for r in originals['libraries']}
    for name,profile in rows.items():
        elf=ROOT/'beatsaber140/lib/arm64-v8a'/(name+'.so');source=work/'build/game-host-unprepared'/(name+'.dylib')
        if bootstrap.digest(elf)!=profile['sha256'] or bootstrap.digest(source)!=originals[name]['unpreparedTranslationSha256']:
            raise ValueError('Game source/translation mismatch: '+name)
        manifest={'schema':1,'elf_sha256':profile['sha256'],'translation_sha256':bootstrap.digest(source),
                  'targets':profile['targets'],'originalEntryDepth':profile['originalEntryDepth']}
        path=output/(name+'-manifest.json');path.write_text(json.dumps(manifest,indent=2)+'\n')
        subprocess.run([sys.executable,str(ROOT/'ports/scotland2/tools/prepare_hooks.py'),'--elf',str(elf),
                        '--input',str(source),'--manifest',str(path),'--output',str(output/(name+'.dylib'))],cwd=ROOT,check=True)
        receipts.append({'library':name,'elfSha256':profile['sha256'],'targets':len(profile['targets']),
                         'preparedTranslationSha256':bootstrap.digest(output/(name+'.dylib'))})
    (output/'preparation-receipt.json').write_text(json.dumps({'libraries':receipts,
        'scope':'Offline unsigned prepared translations; runtime installation and behavior remain untested'},indent=2)+'\n')
    print('Prepared',sum(r['targets'] for r in receipts),'game entry targets offline; no game code executed')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    a=p.parse_args();prepare(a.workspace.resolve())
