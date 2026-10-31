#!/usr/bin/env python3
"""Stage the validated 1.45.3 mod composition as unsigned visionOS frameworks."""
import argparse,json,plistlib,subprocess,sys
from pathlib import Path
import bootstrap
from profile_1453 import selection,ROOT

def validate_receipt(path,work,marker):
    receipt=json.loads(path.read_text())
    if receipt.get('returnCode')!=0 or receipt.get('passed',True) is not True or marker not in (path.parent/'run.log').read_text():
        raise ValueError('Incomplete native validation: '+str(path))
    for row in receipt['modules']:
        if bootstrap.digest(work/'build/libs'/row['name'])!=row['sha256']:
            raise ValueError('Runtime changed after native validation: '+row['name'])

def stage(a):
    chosen=selection();work=a.workspace.resolve();output=a.output.resolve()
    if output.exists():raise ValueError('Use a new generated bundle output; retain prior candidates')
    audit=json.loads((work/'build/artifact-audit.json').read_text())
    proof=json.loads(a.reproducibility.read_text())
    if not audit['passed'] or not proof['passed']:raise ValueError('Artifact/reproducibility validation is incomplete')
    if proof['manifestSha256']!=bootstrap.digest(ROOT/'ports/scotland2/port1453/inputs.json'):
        raise ValueError('Reproducibility proof uses a different source manifest')
    for row in proof['libraries']:
        if not row['identical'] or bootstrap.digest(work/'build/libs'/row['library'])!=row['sha256']:
            raise ValueError('Output changed after independent rebuild: '+row['library'])
    for path,marker in [(a.import_check,'HOST_IMPORTS_AND_CONSTRUCTORS_OK '),
                        (a.early_check,'EARLY_MODS_1453_OK '),(a.download_check,'DOWNLOAD_BOUNDARIES_1453_OK ')]:
        validate_receipt(path.resolve(),work,marker)
    for name,expected in chosen['game']['files'].items():
        path=ROOT/'beatsaber140'/('assets/bin/Data/Managed/Metadata/global-metadata.dat' if name=='global-metadata.dat' else 'lib/arm64-v8a/'+name)
        if bootstrap.digest(path)!=expected:raise ValueError('Owned game input changed: '+name)
    run=lambda args,**kw:subprocess.run([str(x) for x in args],check=True,cwd=ROOT,**kw)
    run(['bash','ports/scotland2/build.sh','xros'],env={**__import__('os').environ,'SC2_BUILD_MODE':'release'})
    metadata=output/'Scotland2Metadata';frameworks=output/'Frameworks';frameworks.mkdir(parents=True)
    def framework(name):
        folder=frameworks/(name+'.framework');folder.mkdir()
        (folder/'Info.plist').write_bytes(plistlib.dumps({'CFBundleExecutable':name,
            'CFBundleIdentifier':'dev.klepton.scotland2.'+name.replace('_','-'),'CFBundleName':name,
            'CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':'26.0'}))
        return folder/name
    rows=[]
    for relative in chosen['metadata']:
        name=Path(relative).name;source=work/'build/libs'/name;destination=metadata/relative
        destination.parent.mkdir(parents=True,exist_ok=True)
        expected=next(row['sha256'] for row in audit['libraries'] if row['name']==name)
        if bootstrap.digest(source)!=expected:raise ValueError('Stale artifact audit: '+name)
        __import__('shutil').copyfile(source,destination)
        target=framework(Path(name).stem)
        run([ROOT/'build/klepton-ld',source,'--platform','visionos','--install-name',
             '@rpath/'+target.parent.name+'/'+target.name,'-o',target,'--quiet'])
        rows.append({'metadata':relative,'elfSha256':expected,'translationSha256':bootstrap.digest(target)})
    target=framework('libcxx_shared');source=work/'build/libs/libc++_shared.so'
    run([ROOT/'build/klepton-ld',source,'--platform','visionos','--install-name','@rpath/libcxx_shared.framework/libcxx_shared','-o',target,'--quiet'])
    core=framework('Scotland2Core');sdk=subprocess.check_output(['xcrun','--sdk','xros','--show-sdk-path'],text=True).strip()
    run(['xcrun','clang++','-target','arm64-apple-xros26.0','-isysroot',sdk,'-dynamiclib',
         '-Wl,-force_load,'+str(ROOT/'build/scotland2/xros/libscotland2-core.a'),
         '-install_name','@rpath/Scotland2Core.framework/Scotland2Core','-o',core])
    certificates=ROOT/'ports/scotland2/certificates';trust=json.loads((certificates/'provenance.json').read_text())
    if bootstrap.digest(certificates/'cacert.pem')!=trust['sha256']:raise ValueError('CA trust asset changed')
    __import__('shutil').copytree(certificates,metadata/'certificates')
    (metadata/'provenance.json').write_text(json.dumps({'schema':1,'profile':chosen['runtimeProfile'],
        'selectionSha256':bootstrap.digest(ROOT/'ports/scotland2/port1453/selected-mods.json'),
        'mode':'release','excludedMods':['FakeId'],'libraries':rows,
        'scope':'Unsigned framework staging only; owned game frameworks, app build/signing and device checks remain separate'},indent=2)+'\n')
    destination=ROOT/'visionos/Frameworks';destination.mkdir(exist_ok=True)
    # New isolated checkout only. Refuse replacing an existing generated output.
    for source in sorted(frameworks.glob('*.framework')):
        name=source.stem;target=destination/(('Scotland2Core' if name=='Scotland2Core' else 'SC2_'+name)+'.xcframework')
        if target.exists():raise ValueError('Generated framework already exists; preserve or archive it first: '+str(target))
        run(['xcodebuild','-create-xcframework','-framework',source,'-output',target],stdout=subprocess.DEVNULL)
    target=ROOT/'visionos/Scotland2Metadata'
    if target.exists():raise ValueError('Generated metadata already exists; preserve the prior candidate')
    __import__('shutil').copytree(metadata,target)
    print('STAGED_1453_FRAMEWORKS_OK exact 23-module metadata composition, shared Android runtime and original native Scotland2 core; unsigned, not installed')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__)
    for name in ['workspace','output','reproducibility','import-check','early-check','download-check']:
        p.add_argument('--'+name,type=Path,required=True)
    stage(p.parse_args())
