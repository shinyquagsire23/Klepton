#!/usr/bin/env python3
"""Reconstruct pinned Unity 6.3 mod sources and SDK aliases in a new workspace.

Public sources and patches only. This does not copy games, saves or signing data,
package an application, install anything, or establish runtime compatibility.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import zipfile
import bootstrap

ROOT=Path(__file__).resolve().parents[3]
MANIFEST=ROOT/'ports/scotland2/port1453/inputs.json'
PATCHES=ROOT/'ports/scotland2/port1453/patches'

def clone(source,destination):
    if destination.exists(): raise ValueError('Preparation requires a new destination: '+str(destination))
    if __import__('sys').platform=='darwin': subprocess.run(['cp','-cR',str(source),str(destination)],check=True)
    else: shutil.copytree(source,destination)

def link(path,target):
    if path.is_dir() and not path.is_symlink() and not any(path.iterdir()): path.rmdir()
    if path.exists() or path.is_symlink():
        if not path.is_symlink() or path.resolve()!=target.resolve(): raise ValueError('SDK alias conflicts: '+str(path))
        return
    path.parent.mkdir(parents=True,exist_ok=True);path.symlink_to(target,target_is_directory=True)

def prepare(work,cache,seeds,legacy,offline):
    manifest=json.loads(MANIFEST.read_text()); originals=work/'sources'; adapted=work/'work-sources'
    if originals.exists() or adapted.exists(): raise ValueError('Use a new workspace to preserve existing source work')
    originals.mkdir(parents=True);adapted.mkdir()
    rows=list(manifest['sources'])
    # Reuse the existing independently selected Git blob archive, without the
    # upstream repository's bundled OpenSSL/build tree. The new patch is applied
    # to pristine 2.0.2 files, never to the already adapted 1.40 source.
    airbuds=next(x for x in json.loads((ROOT/'ports/scotland2/source-inputs.json').read_text())['sources']
                 if x['target']=='ports/scotland2/airbuds-search/upstream')
    rows.append({**airbuds,'name':'airbuds-search'})
    receipts=[]
    for row in rows:
        name=row['name'];print('Reconstruct',name,flush=True)
        archive=bootstrap.fetch(row,cache,seeds,offline)
        bootstrap.unpack(archive,originals/name,1)
        patch=PATCHES/(name+'.patch')
        if patch.exists():
            expected=next(r['patchSha256'] for r in manifest['patches'] if r['module']==name)
            if bootstrap.digest(patch)!=expected: raise ValueError('Changed reconstruction patch: '+name)
            clone(originals/name,adapted/name)
            subprocess.run(['patch','--batch','--forward','-d',str(adapted/name),'-p1','-i',str(patch)],
                           check=True,stdout=subprocess.DEVNULL)
        else: clone(originals/name,adapted/name)
        # Klepton's own follow-up changes (e.g. airbuds-search-applemusic.patch), after the
        # recorded reconstruction patch and outside its manifest, so the reference stays valid.
        for extra in sorted(PATCHES.glob(name+'-*.patch')):
            subprocess.run(['patch','--batch','--forward','-d',str(adapted/name),'-p1','-i',str(extra)],
                           check=True,stdout=subprocess.DEVNULL)
        receipts.append({'name':name,'sourceSha256':row['sha256'],
                         'patchSha256':bootstrap.digest(patch) if patch.exists() else None,
                         'followUpPatches':{p.name:bootstrap.digest(p) for p in sorted(PATCHES.glob(name+'-*.patch'))}})
    libil2cpp={**manifest['libil2cpp'],'file':'libil2cpp_0.5.0.zip'}
    archive=bootstrap.fetch(libil2cpp,cache,seeds,offline)
    bootstrap.unpack(archive,originals/'libil2cpp',0)
    sdk=work/'build/sdk';sdk.mkdir(parents=True)
    aliases={'beatsaber-hook':'bshook','custom-types':'customtypes','bsml':'bsml','lapiz':'lapiz',
             'songcore':'songcore','metacore':'metacore','metacore-bs':'metacore-bs',
             'playlistcore':'playlistcore','flamingo':'flamingo','web-utils':'web-utils',
             'song-details':'song-details','beatsaverplusplus':'beatsaverplusplus',
             'paper2_scotland2':'paperlog','config-utils':'config-utils','rapidjson-macros':'rapidjson-macros'}
    for alias,name in aliases.items(): link(sdk/alias,adapted/name)
    for prefix in ['shared','include']: link(sdk/'reflectcpp'/prefix,originals/'reflectcpp/include')
    link(sdk/'scotland2',ROOT/'ports/scotland2/upstream')
    link(sdk/'capstone',legacy/'capstone-qpm/source')
    for name in ['gif-lib','tinyxml2','rapidjson']: link(sdk/name,legacy/name)
    link(sdk/'libcurl',legacy/'libcurl-8.5.0')
    link(sdk/'fmt/fmt/include',adapted/'fmt/include')
    link(adapted/'bshook/shared/rapidjson',legacy/'rapidjson')
    link(adapted/'paperlog/shared/utfcpp',legacy/'utfcpp')
    for name in ['EasyGifReader','cppcodec','css-color-parser-cpp']:
        link(adapted/'bsml/include/submodules'/name,legacy/name)
    # The compiler consumes bs-cordl from the pristine pinned codegen directory.
    # Read-only original QMOD ELFs supply declared dependencies, not port results.
    elfs=work/'elf';elfs.mkdir()
    for reference in manifest.get('linkReferences',[]):
        source=bootstrap.fetch(reference,cache,seeds,offline)
        shutil.copyfile(source,elfs/reference['file'])
    for package in manifest['packages']:
        archive=bootstrap.fetch(package,cache,seeds,offline)
        members=package['libraryFiles']+package['earlyModFiles']+package['lateModFiles']
        with zipfile.ZipFile(archive) as zipped:
            for name in members:
                if Path(name).name!=name: raise ValueError('Unexpected nested QMOD filename')
                matches=[m for m in zipped.namelist() if Path(m).name==name]
                if len(matches)!=1: raise ValueError('Ambiguous QMOD member: '+name)
                content=zipped.read(matches[0]);dest=elfs/name
                if dest.exists() and dest.read_bytes()!=content: raise ValueError('Conflicting original library: '+name)
                dest.write_bytes(content)
    (work/'source-reconstruction-receipt.json').write_text(json.dumps({
        'manifestSha256':bootstrap.digest(MANIFEST),'sources':receipts,
        'scope':'Hash-verified source/SDK reconstruction only; compiler, native support, game hooks and packaging remain separate'},indent=2)+'\n')
    print('Prepared pinned sources and SDK; no application built or installed',flush=True)

if __name__=='__main__':
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--workspace',type=Path,required=True);parser.add_argument('--cache',type=Path,required=True)
    parser.add_argument('--legacy-dependencies',type=Path,required=True);parser.add_argument('--seed',type=Path,action='append',default=[])
    parser.add_argument('--offline',action='store_true');args=parser.parse_args()
    prepare(args.workspace.resolve(),args.cache.resolve(),[x.resolve() for x in args.seed],args.legacy_dependencies.resolve(),args.offline)
