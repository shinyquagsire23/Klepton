#!/usr/bin/env python3
"""Audit the explicit release inventory, public API and diagnostic exclusions."""
import argparse,json,struct,sys
from pathlib import Path
import bootstrap
from build_1453_module import MODULES
ROOT=Path(__file__).resolve().parents[3]
sys.path.insert(0,str(ROOT/'tools'))
from audit_elf import Elf,cstr
APIS={'libbeatsaber-hook.so':['i2c','get_data_dir','get_config_path','mkpath','readfile','writefile'],
      'libcustom-types.so':['custom_types'],'liblapiz.so':['Lapiz'],'libbsml.so':['BSML'],
      'libmetacore.so':['MetaCore'],'libmetacore-bs.so':['MetaCore'],'libsongcore.so':['SongCore'],
      'libplaylistcore.so':['PlaylistCore'],'libsongdownloader.so':['BeatSaver','ScoreSaber'],
      'libsongdetails.so':['SongDetailsCache'],'libweb-utils.so':['WebUtils']}
def strong(elf):
    result=set()
    for table in elf.sh:
        if table[1]!=11:continue
        strings=elf.sh[table[6]][4]
        for at in range(table[4],table[4]+table[5],24):
            n,info,other,index,address,size=struct.unpack_from('<IBBHQQ',elf.data,at)
            if index and info>>4==1 and other&3==0:result.add(cstr(elf.data,strings+n))
    return result
def check(work):
    selection=json.loads((ROOT/'ports/scotland2/port1453/selected-mods.json').read_text())
    expected={Path(name).name for name in selection['metadata']}|{'libc++_shared.so'}
    if len(expected)!=24 or any('fakeid' in name.lower() for name in expected):raise ValueError('Unexpected release inventory')
    built={value[2]+'.so' for value in MODULES.values()}|{'libflamingo.so','libsl2_abi.so','libsc2_scene.so'}
    rows=[];api=[]
    for name in sorted(expected):
        path=work/'build/libs'/name;elf=Elf(str(path));imports={n for n,_ in elf.imports}
        if not elf.ok:raise ValueError('Invalid guest ELF: '+name)
        if any(n.startswith('sc2_test_') for n in elf.exports):raise ValueError('Diagnostic export: '+name)
        if name in built:
            forbidden={'system','popen','mprotect','__clear_cache'}
            if name in {'libweb-utils.so','libsongdetails.so','libsongdownloader.so'}:forbidden.remove('mprotect')
            if imports&forbidden:raise ValueError('Executable-write/shell dependency: '+name+str(imports&forbidden))
            if any(table[1]==2 for table in elf.sh):raise ValueError('Private symbol table left in release: '+name)
        if name=='libcustom-types.so' and b'CustomTypesMallocInfoOnExit.xml' in elf.data:raise ValueError('Custom-types GC tracing enabled')
        rows.append({'name':name,'sha256':bootstrap.digest(path),'needed':elf.needed,'sourceBuilt':name in built})
    for name,patterns in APIS.items():
        original=Elf(str(work/'elf'/name));ported=Elf(str(work/'build/libs'/name))
        required={n for n in strong(original) if any(p in n for p in patterns)}
        missing=sorted(required-ported.exports)
        api.append({'library':name,'requiredStrongNamespaceSymbols':len(required),'missing':missing})
    original=Elf(str(work/'elf/libbeatsaverplusplus.so'));ported=Elf(str(work/'build/libs/libbeatsaverplusplus.so'))
    required={name for name in original.exports if 'BeatSaver' in name}
    missing=sorted(required-ported.exports)
    api.append({'library':'libbeatsaverplusplus.so','requiredPublishedSymbols':len(required),'missing':missing})
    # Supporting WebUtils/STL RTTI is supplied by this DSO or the shared NDK
    # runtime; its incidental original location is not part of BeatSaver's API.
    standard=original.exports-required
    runtime=Elf(str(work/'build/libs/libc++_shared.so'))
    missing_support=standard-ported.exports-runtime.exports
    if missing_support:raise ValueError('Supporting runtime ABI missing: '+str(missing_support))
    passed=not any(row['missing'] for row in api)
    (work/'build/artifact-audit.json').write_text(json.dumps({'passed':passed,'libraries':rows,'publicApi':api,
        'supportingRuntimeSymbols':sorted(standard),
        'scope':'Static inventory/API/import policy and diagnostic exclusions; native runtime/device checks are separate'},indent=2)+'\n')
    if not passed:raise ValueError('Public API missing: '+str([row for row in api if row['missing']]))
    print('ARTIFACT_AUDIT_1453_OK',len(rows),'selected guest libraries;',sum(row.get('requiredStrongNamespaceSymbols',0) for row in api),
          'original strong API symbols and',len(required),'BeatSaverPlusPlus API exports preserved; supporting RTTI retained in its module/shared runtime; no Fake ID, diagnostic exports or private symbols in source-built release ELFs')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    a=p.parse_args();check(a.workspace.resolve())
