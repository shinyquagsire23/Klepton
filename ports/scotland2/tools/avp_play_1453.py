#!/usr/bin/env python3
"""AVP Play adapter for the verified Unity 6.3 workspace; no account/game payloads."""
import argparse,json,os,shutil,subprocess,sys,zipfile,hashlib
from pathlib import Path
import bootstrap
from profile_1453 import ROOT,selection
from avp_play import install_app,validate_metadata,profile_configuration
from stage_1453_bundle import validate_receipt

def completed_check(work,pattern,marker):
    paths=sorted((work/'build').glob(pattern+'/validation.json'),key=lambda path:path.stat().st_mtime_ns,reverse=True)
    for path in paths:
        try:validate_receipt(path,work,marker);return path
        except (ValueError,OSError,KeyError):continue
    raise ValueError('No completed validation matches the current libraries: '+pattern)

def validate_game(root):
    if validate_metadata(root)!=3237:raise ValueError('This adapter requires versionCode 3237')
    game=selection()['game'];apk=root/'beatsaber-1.40.apk'
    with zipfile.ZipFile(apk) as zipped:
        for name,expected in game['files'].items():
            relative='assets/bin/Data/Managed/Metadata/global-metadata.dat' if name=='global-metadata.dat' else 'lib/arm64-v8a/'+name
            path=root/'beatsaber140'/relative
            if bootstrap.digest(path)!=expected:raise ValueError('Unsupported owned game input: '+name)
            if zipped.namelist().count(relative)!=1:raise ValueError('Ambiguous/missing APK member: '+relative)
            digest=hashlib.sha256()
            with zipped.open(relative) as source:
                for data in iter(lambda:source.read(4<<20),b''):digest.update(data)
            if digest.hexdigest()!=expected:raise ValueError('APK differs from extracted game: '+name)

def main():
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--no-install',action='store_true');a=parser.parse_args()
    if os.environ.get('KLEPTON_TARGET')!='beatsaber140':raise ValueError('Unsupported AVP target')
    team=os.environ.get('KLEPTON_TEAM');bundle=os.environ.get('KLEPTON_BUNDLE_ID');device=os.environ.get('KLEPTON_DEVICE')
    if not team or bundle!='dev.klepton.beatsaber140.scotland2.test' or (not a.no_install and not device):
        raise ValueError('Use the existing Scotland2 Test app identity, Apple team and connected device')
    validate_game(ROOT)
    config=profile_configuration(ROOT).get('port1453',{})
    if not config.get('workspace'):
        raise ValueError('Configure port1453.workspace in .scotland2-avp-local.json with the independently prepared build inputs described in ports/scotland2/port1453/README.md')
    work=Path(config['workspace']).expanduser().resolve()
    proof=Path(config.get('reproducibility',work/'repro-final/build/reproducibility.json')).resolve()
    imports=Path(config['importCheck']).resolve() if config.get('importCheck') else completed_check(work,'mac-import-check*','HOST_IMPORTS_AND_CONSTRUCTORS_OK ')
    early=Path(config['earlyCheck']).resolve() if config.get('earlyCheck') else completed_check(work,'mac-early-check*','EARLY_MODS_1453_OK ')
    downloads=Path(config['downloadCheck']).resolve() if config.get('downloadCheck') else completed_check(work,'mac-download-check*','DOWNLOAD_BOUNDARIES_1453_OK ')
    receipt=json.loads(proof.read_text())
    if not receipt['passed'] or receipt['manifestSha256']!=bootstrap.digest(ROOT/'ports/scotland2/port1453/inputs.json'):
        raise ValueError('Require a completed independent rebuild for the selected sources')
    for row in receipt['libraries']:
        if not row['identical'] or bootstrap.digest(work/'build/libs'/row['library'])!=row['sha256']:
            raise ValueError('Library changed after independent validation: '+row['library'])
    for path,marker in [(imports,'HOST_IMPORTS_AND_CONSTRUCTORS_OK '),(early,'EARLY_MODS_1453_OK '),(downloads,'DOWNLOAD_BOUNDARIES_1453_OK ')]:
        validate_receipt(path,work,marker)
    run=lambda args,cwd=ROOT:subprocess.run([str(x) for x in args],cwd=cwd,check=True)
    run(['make','build/klepton-ld'])
    metadata=ROOT/'visionos/Scotland2Metadata'
    if not metadata.exists():
        output=work/'build'/('avp-visionos-mods-'+hashlib.sha256(str(ROOT).encode()).hexdigest()[:12])
        run([sys.executable,ROOT/'ports/scotland2/tools/stage_1453_bundle.py','--workspace',work,'--output',output,
             '--reproducibility',proof,'--import-check',imports,'--early-check',early,'--download-check',downloads])
    # AVP Play prepares artwork/APK metadata from the selected imported recipe.
    # Prepare game frameworks only if this source checkout has not done so yet.
    game_preparation=Path(config.get('gamePreparation',work/'build'/('avp-game-'+hashlib.sha256(str(ROOT).encode()).hexdigest()[:12]))).resolve()
    if not game_preparation.exists():
        env=dict(os.environ,XROS_PLATFORMS='xros')
        subprocess.run(['bash','visionos/mkguest.sh'],cwd=ROOT,env=env,check=True)
        run([sys.executable,ROOT/'ports/scotland2/tools/stage_1453_game.py','--workspace',work,'--output',game_preparation])
    run([sys.executable,ROOT/'ports/scotland2/tools/assemble_1453_app.py','--workspace',work,
         '--game-preparation',game_preparation,'--team',team,'--bundle-id',bundle,
         '--build-number',os.environ.get('KLEPTON_APP_BUILD','349')])
    app=ROOT/'visionos/build/port1453-release-dd/Build/Products/Release-xros/Klepton.app'
    destination=ROOT/'visionos/build/dd-device-beatsaber140/Build/Products/Debug-xros/Klepton.app'
    destination.parent.mkdir(parents=True,exist_ok=True)
    if destination.is_symlink():raise ValueError('AVP app destination is a link')
    if destination.exists():shutil.rmtree(destination)
    run(['cp','-cR',app,destination]);run(['codesign','--verify','--deep','--strict',destination])
    if a.no_install:
        print('SCOTLAND2_AVP_1453_READY original core/search implementations; icon and start/settings UI; signature verified; NOT installed',flush=True)
        return
    install_app(ROOT,device,destination)
    print('SCOTLAND2_AVP_1453_INSTALLED original AVP recipe will stage its matching OBB and add-ons; no game launch/data reset',flush=True)
if __name__=='__main__':
    try:main()
    except (ValueError,OSError,KeyError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
