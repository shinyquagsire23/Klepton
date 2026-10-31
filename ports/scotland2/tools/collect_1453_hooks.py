#!/usr/bin/env python3
"""Bind compiler hook catalogs and measured native targets to the owned 1.45.3 ELFs."""
import argparse,hashlib,json,struct,sys
from pathlib import Path
from read_hook_catalog import read
from find_customtypes_targets import profile_unity63
from find_scene_target import find as scene_profile
from build_1453_module import validate_unity_profile
ROOT=Path(__file__).resolve().parents[3]
CATALOGS={'lapiz':'liblapiz.so','bsml':'libbsml.so','songcore':'libsongcore.so','metacore-bs':'libmetacore-bs.so',
          'playlistcore':'libplaylistcore.so','datakeeper':'libdatakeeper.so','unicode':'libunicode.so',
          'bettersongsearch':'libBetterSongSearch.so','airbuds-search':'libairbuds-search.so'}
def digest(path):return hashlib.sha256(path.read_bytes()).hexdigest()
def collect(work,game):
    selected=json.loads((ROOT/'ports/scotland2/port1453/inputs.json').read_text())
    il2cpp=game/'lib/arm64-v8a/libil2cpp.so';unity=game/'lib/arm64-v8a/libunity.so'
    metadata=game/'assets/bin/Data/Managed/Metadata/global-metadata.dat'
    paths={'libil2cpp.so':il2cpp,'libunity.so':unity,'global-metadata.dat':metadata}
    for name,path in paths.items():
        if digest(path)!=selected['game']['files'][name]:raise ValueError('Unsupported game file: '+name)
    output=work/'build/hooks-1453';output.mkdir(exist_ok=True)
    custom=profile_unity63(il2cpp,unity)
    metacore=validate_unity_profile(ROOT/'ports/scotland2/port1453/metacore-unity-targets.json',unity)
    scene=scene_profile(unity)
    (output/'customtypes-targets.json').write_text(json.dumps(custom,indent=2)+'\n')
    (output/'scene-target.json').write_text(json.dumps(scene,indent=2)+'\n')
    catalogs={}
    for name,filename in CATALOGS.items():
        data=read(work/'build/libs'/filename,il2cpp)
        catalogs[name]=data
        (output/(name+'-catalog.json')).write_text(json.dumps(data,indent=2)+'\n')
    combined={int(t['vaddr'],0):dict(t) for t in custom['il2cpp']['targets']}
    for name,data in catalogs.items():
        for target in data['targets']:
            address=int(target['vaddr'],0)
            if address in combined:
                if int(combined[address]['expect'],0)!=int(target['expect'],0):raise ValueError('Conflicting original instruction')
                combined[address]['name']+=' / '+name+':'+target['name']
            else:combined[address]={**target,'name':name+':'+target['name']}
    result={'schema':1,'game':selected['game'],
            'il2cpp':{'sha256':digest(il2cpp),'targets':[combined[a] for a in sorted(combined)],
                       'originalEntryDepth':max(d['originalEntryDepth'] for d in catalogs.values())},
            'customtypesUnity':custom['unity'],'metacoreUnity':metacore,
            'sceneUnity':scene,
            'moduleHashes':{name:d['modSha256'] for name,d in catalogs.items()},
            'scope':'Measured and compiler-resolved targets only; no signed preparation, runtime hook installation or game success is implied'}
    (output/'combined-targets.json').write_text(json.dumps(result,indent=2)+'\n')
    print(len(combined),'unique IL2CPP targets from',len(catalogs),'original module catalogs; Unity targets retained separately')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--game-dir',type=Path,default=ROOT/'beatsaber140');a=p.parse_args()
    collect(a.workspace.resolve(),a.game_dir.resolve())
