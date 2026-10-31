#!/usr/bin/env python3
"""Build the staged 1.45.3 candidate with the existing test identity and launcher."""
import argparse,json,os,plistlib,re,shlex,shutil,subprocess,sys
from pathlib import Path
import bootstrap
from profile_1453 import selection,ROOT
from build_release import stage_presentation,INSTALLER_DEFAULTS
EXISTING_APP_ID='dev.klepton.beatsaber140.scotland2.test'

def build_stamp(value):
    value=str(value)
    if not re.fullmatch(r'[0-9]{1,9}(?:\.[0-9]{1,9}){0,2}',value):
        raise argparse.ArgumentTypeError('Use the numeric AVP build stamp, for example 3237.351.1')
    return value

def framework_binary(container):
    info=plistlib.loads((container/'Info.plist').read_bytes())
    rows=[row for row in info['AvailableLibraries'] if row.get('SupportedPlatform')=='xros' and not row.get('SupportedPlatformVariant')]
    if len(rows)!=1:raise ValueError('Require one visionOS device framework slice')
    folder=container/rows[0]['LibraryIdentifier']/rows[0]['LibraryPath']
    return folder/folder.stem

def assemble(a):
    chosen=selection();work=a.workspace.resolve();prepared=a.game_preparation.resolve()
    if a.bundle_id!=EXISTING_APP_ID or not re.fullmatch(r'[A-Z0-9]{10}',a.team):
        raise ValueError('Use the existing Scotland2 Test application identity and explicit Apple team')
    metadata=ROOT/'visionos/Scotland2Metadata'
    provenance=json.loads((metadata/'provenance.json').read_text())
    if provenance['profile']!='1.45.3' or provenance['selectionSha256']!=bootstrap.digest(ROOT/'ports/scotland2/port1453/selected-mods.json'):
        raise ValueError('Stage the current 1.45.3 mod composition first')
    if {str(p.relative_to(metadata)) for p in metadata.rglob('*.so')}!=set(chosen['metadata']):
        raise ValueError('Stale/missing mod metadata')
    for row in provenance['libraries']:
        path=metadata/row['metadata'];name=path.stem
        if bootstrap.digest(path)!=row['elfSha256'] or bootstrap.digest(work/'build/libs'/path.name)!=row['elfSha256']:
            raise ValueError('Staged guest ELF changed: '+name)
        binary=framework_binary(ROOT/'visionos/Frameworks'/('SC2_'+name+'.xcframework'))
        if bootstrap.digest(binary)!=row['translationSha256']:raise ValueError('Staged guest translation changed: '+name)
    game_receipt=json.loads((prepared/'preparation-receipt.json').read_text())
    for row in game_receipt['libraries']:
        name=row['library'];raw=ROOT/'beatsaber140/lib/arm64-v8a'/(name+'.so')
        binary=framework_binary(ROOT/'visionos/Frameworks/beatsaber140'/(name+'.xcframework'))
        if bootstrap.digest(raw)!=row['elfSha256'] or bootstrap.digest(binary)!=row['preparedTranslationSha256']:
            raise ValueError('Staged owned game translation changed: '+name)
    metadata_version=(ROOT/'beatsaber140/apktool.yml').read_text()
    if not re.search(r'^\s*versionCode:\s*[\'"]?3237[\'"]?\s*$',metadata_version,re.M):
        raise ValueError('Startup metadata must identify versionCode 3237')
    for row in json.loads((ROOT/'ports/scotland2/graphics-inputs.json').read_text())['files']:
        if bootstrap.digest(ROOT/row['path'])!=row['sha256']:raise ValueError('Graphics cache changed: '+row['path'])
    if a.presentation_assets:
        if a.presentation_assets.resolve()==(ROOT/'visionos/Assets.xcassets').resolve():raise ValueError('Presentation input must be separate')
        stage_presentation(a.presentation_assets.resolve(),ROOT)
        shutil.move(str(ROOT/'presentation-receipt.json'),work/'build/visionos-app-artwork.json')
    env={key:value for key,value in os.environ.items() if not key.startswith(('KLEPTON_','SC2_','KL_'))}
    env.update({key:os.environ[key] for key in INSTALLER_DEFAULTS if key in os.environ})
    env.update(PYTHONPYCACHEPREFIX='/private/tmp/sc2-pycache',KLEPTON_TARGET='beatsaber140',KLEPTON_SCOTLAND2='1',
        SC2_BUILD_MODE='release',SC2_GAME_PROFILE='1.45.3',SC2_TEST_SET='coremods',XROS_PLATFORMS='xros',
        KLEPTON_TEAM=a.team,KLEPTON_BUNDLE_ID=a.bundle_id,KLEPTON_ENTITLEMENTS='0',KLEPTON_START_SCREEN='1',
        KLEPTON_DISPLAY_OVERRIDE='Beat Saber (Custom Songs)',KLEPTON_APP_VERSION='1.45.3',KLEPTON_APP_BUILD=str(a.build_number),
        KLEPTON_EVA='0',KLEPTON_LOW_LATENCY='0',KL_GUEST_JIT='0')
    env['REPRO_FLAGS']=shlex.join(['-ffile-prefix-map='+str(ROOT)+'=.','-fmacro-prefix-map='+str(ROOT)+'=.','-fdebug-compilation-dir=.'])
    def run(args,cwd=ROOT):subprocess.run([str(x) for x in args],cwd=cwd,env=env,check=True)
    run(['make','KLEPTON_SCOTLAND2=1','SC2_BUILD_MODE=release','XROS_PLATFORMS=xros','xros'])
    for script in ['mkangle.sh','mkmvk.sh']:run(['bash',script],ROOT/'visionos')
    run([sys.executable,'gen_xcodeproj.py'],ROOT/'visionos')
    derived='build/port1453-release-dd'
    command=['xcodebuild','-project','Klepton.xcodeproj','-scheme','Klepton','-configuration','Release',
             '-destination','generic/platform=visionOS','-derivedDataPath',derived]
    command+=['CODE_SIGNING_ALLOWED=NO'] if a.unsigned else ['-allowProvisioningUpdates']
    run([*command,'build'],ROOT/'visionos')
    app=ROOT/'visionos'/derived/'Build/Products/Release-xros/Klepton.app'
    info=plistlib.loads((app/'Info.plist').read_bytes())
    for key,value in {'CFBundleIdentifier':a.bundle_id,'KLScotland2Profile':'1.45.3','KLScotland2Diagnostics':'0',
                      'KLAllowTestSwitches':'0','KLTestBuild':'0','KLStartScreen':'1'}.items():
        if str(info.get(key))!=value:raise ValueError('Candidate app setting mismatch: '+key)
    if a.presentation_assets and info.get('CFBundleIconName')=='AppIcon':raise ValueError('Owned game icon was not compiled')
    actual={str(p.relative_to(app/'Scotland2Metadata')) for p in (app/'Scotland2Metadata').rglob('*.so')}
    if actual!=set(chosen['metadata']):raise ValueError('App mod inventory differs from selected composition')
    for relative in chosen['metadata']:
        if bootstrap.digest(app/'Scotland2Metadata'/relative)!=bootstrap.digest(metadata/relative):raise ValueError('App metadata changed: '+relative)
    if any('fakeid' in str(p.relative_to(app)).lower() or '_probe' in p.name or p.name.endswith('.hooks.json') for p in app.rglob('*')):
        raise ValueError('Excluded or diagnostic material in candidate app')
    if not a.unsigned:run(['codesign','--verify','--deep','--strict',app])
    report={'bundleId':a.bundle_id,'version':'1.45.3','build':a.build_number,'signed':not a.unsigned,
        'metadata':sorted(actual),'iconName':info.get('CFBundleIconName'),'startSettingsScreenEnabled':True,
        'releaseControlsDisabled':True,'fakeIdAbsent':True,
        'files':{str(p.relative_to(app)):bootstrap.digest(p) for p in app.rglob('*') if p.is_file()},
        'scope':'Local candidate app built and structurally validated; not installed or run on the headset'}
    (work/'build/visionos-app-validation.json').write_text(json.dumps(report,indent=2)+'\n')
    print('CANDIDATE_1453_APP_OK',app,'signed='+str(not a.unsigned),'not installed')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--game-preparation',type=Path,required=True);p.add_argument('--presentation-assets',type=Path)
    p.add_argument('--team',required=True);p.add_argument('--bundle-id',default=EXISTING_APP_ID)
    p.add_argument('--build-number',type=build_stamp,default='349');p.add_argument('--unsigned',action='store_true')
    assemble(p.parse_args())
