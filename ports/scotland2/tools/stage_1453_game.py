#!/usr/bin/env python3
"""Translate/prepare exact owned game images offline for the isolated 1.45.3 app."""
import argparse,json,plistlib,shutil,subprocess,sys
from pathlib import Path
import bootstrap
from profile_1453 import selection,ROOT
from prepare_1453_game_hooks import game_profiles
def stage(work,output):
    chosen=selection();combined=json.loads((work/'build/hooks-1453/combined-targets.json').read_text())
    if combined['game']!=chosen['game']:raise ValueError('Game hook catalog uses a different game')
    for module,expected in combined['moduleHashes'].items():
        from collect_1453_hooks import CATALOGS
        if bootstrap.digest(work/'build/libs'/CATALOGS[module])!=expected:raise ValueError('Stale compiled hook catalog: '+module)
    if output.exists():raise ValueError('Use a new generated game output; preserve prior candidates')
    output.mkdir(parents=True);rows=[]
    run=lambda args:subprocess.run([str(x) for x in args],cwd=ROOT,check=True)
    for name,profile in game_profiles(combined).items():
        raw=ROOT/'beatsaber140/lib/arm64-v8a'/(name+'.so')
        if bootstrap.digest(raw)!=profile['sha256']:raise ValueError('Unsupported owned game ELF: '+name)
        plain=output/(name+'.unprepared.dylib');framework=output/(name+'.framework');framework.mkdir()
        run([ROOT/'build/klepton-ld',raw,'--platform','visionos','--install-name','@rpath/'+framework.name+'/'+name,'-o',plain,'--quiet'])
        manifest={'schema':1,'elf_sha256':profile['sha256'],'translation_sha256':bootstrap.digest(plain),
                  'targets':profile['targets'],'originalEntryDepth':profile['originalEntryDepth']}
        path=output/(name+'-manifest.json');path.write_text(json.dumps(manifest,indent=2)+'\n')
        run([sys.executable,ROOT/'ports/scotland2/tools/prepare_hooks.py','--elf',raw,'--input',plain,
             '--manifest',path,'--output',framework/name])
        # Gates/records are embedded in the Mach-O. Keep the preparation audit
        # beside the private build, outside the framework shipped to the app.
        (framework/(name+'.hooks.json')).rename(output/(name+'.hooks.json'))
        (framework/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,
            'CFBundleIdentifier':'dev.klepton.scotland2.game.'+name,'CFBundleName':name,
            'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':'26.0'}))
        target=ROOT/'visionos/Frameworks/beatsaber140'/(name+'.xcframework');target.parent.mkdir(parents=True,exist_ok=True)
        if target.exists():
            # mkguest.sh's unprepared copy is generated in this isolated checkout.
            # Preserve it beside the new candidate rather than discarding it.
            prior=output/'previous-unprepared';prior.mkdir(exist_ok=True);shutil.move(str(target),prior/target.name)
        run(['xcodebuild','-create-xcframework','-framework',framework,'-output',target])
        rows.append({'library':name,'elfSha256':profile['sha256'],'targets':len(profile['targets']),
                     'preparedTranslationSha256':bootstrap.digest(framework/name)})
    (output/'preparation-receipt.json').write_text(json.dumps({'libraries':rows,
        'scope':'Unsigned owned game translations prepared offline; no Unity startup or headset execution'},indent=2)+'\n')
    print('STAGED_1453_GAME_HOOKS_OK',sum(row['targets'] for row in rows),'measured game entry targets prepared before signing; no game code executed')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--output',type=Path,required=True);a=p.parse_args();stage(a.workspace.resolve(),a.output.resolve())
