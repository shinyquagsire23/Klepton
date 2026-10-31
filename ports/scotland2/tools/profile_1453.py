"""The bounded Unity 6.3 composition shared by staging and AVP Play."""
import json
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3]
def selection():
    folder=ROOT/'ports/scotland2/port1453'
    chosen=json.loads((folder/'selected-mods.json').read_text())
    inputs=json.loads((folder/'inputs.json').read_text())
    if chosen['game']!=inputs['game'] or chosen['runtimeProfile']!='1.45.3':raise ValueError('1.45.3 profile/game mismatch')
    expected=[]
    for package in inputs['packages']:
        for field,phase in [('libraryFiles','libs'),('earlyModFiles','early_mods'),('lateModFiles','mods')]:
            expected += [phase+'/'+name for name in package[field] if name!='libflamingo.so']
    expected += ['mods/libairbuds-search.so','shims/libsl2_abi.so','shims/libflamingo.so','shims/libsc2_scene.so']
    if len(chosen['metadata'])!=23 or len(set(chosen['metadata']))!=23 or set(chosen['metadata'])!=set(expected):
        raise ValueError('1.45.3 requires its exact pinned runtime inventory')
    if chosen['excludedMods']!=['FakeId'] or any('fakeid' in name.lower() for name in chosen['metadata']):
        raise ValueError('Fake ID must remain excluded')
    return chosen
