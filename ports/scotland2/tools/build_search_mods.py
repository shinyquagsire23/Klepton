#!/usr/bin/env python3
"""Build original search-mod sources/assets against the pinned shared port ABI."""
import concurrent.futures,hashlib,json,os,pathlib,re,subprocess,sys
from build_flags import reproducible_flags
ROOT=pathlib.Path(__file__).resolve().parents[3]

def main():
 name=sys.argv[1]
 if name not in {'bettersongsearch','airbuds-search'}:raise ValueError('Unknown search mod')
 os.chdir(ROOT);src=ROOT/'ports/scotland2'/name/'upstream';out=ROOT/'build/scotland2'/('android-'+name)
 out.mkdir(parents=True,exist_ok=True);objects=out/'objects';objects.mkdir(exist_ok=True)
 sdk=ROOT/'build/scotland2/mod-sdk';dep=ROOT/'port-deps/sources'
 ndk=pathlib.Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
 tools=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin';cxx=tools/'aarch64-linux-android26-clang++'
 for key,target in [('metacore',ROOT/'ports/scotland2/metacore/upstream'),('config-utils',dep/'config-utils'),('song-details',ROOT/'ports/scotland2/song-details/upstream')]:
  link=sdk/key
  if not link.exists():link.symlink_to(target,target_is_directory=True)
  if link.resolve()!=target.resolve():raise ValueError('Shared SDK mismatch: '+key)
 # No independent OpenSSL or unused legacy password-encryption unit is needed:
 # the original Airbuds client uses shared WebUtils TLS and only decoded JWT
 # payloads through OpenSSL. The bounded equivalent is in search-port-support.
 sources=sorted((src/'src').rglob('*.cpp'))
 if name=='airbuds-search':sources=[p for p in sources if p.name!='Encryption.cpp']
 if not sources:raise ValueError('Original sources are missing')
 includes=[out/'include',src,src/'include',src/'src',sdk,dep/'bs-cordl/include',
           dep/'libil2cpp/il2cpp/libil2cpp',dep/'libil2cpp/il2cpp/external/baselib/Include',
           dep/'libil2cpp/il2cpp/external/baselib/Platforms/Android/Include',
           dep/'fmt/include',dep/'utfcpp/source',ROOT/'ports/scotland2/src']
 includes[0].mkdir(exist_ok=True)
 asm=['.section .data.sc2_assets,"aw",@progbits'];declarations=['#pragma once','#include "metacore/shared/assets.hpp"','namespace IncludedAssets { namespace Externs {']
 assets=sorted(p for p in (src/'assets').rglob('*') if p.is_file())
 for asset in assets:
  rel=str(asset.relative_to(src/'assets'));symbol='_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',rel)
  asm+=['.balign 8','.global '+symbol+'_start','.global '+symbol+'_end',symbol+'_start:',
        '.space 32,32','.incbin '+json.dumps(str(asset)),'.byte 32',symbol+'_end:']
  declarations+=['extern "C" uint8_t '+symbol+'_start[];', 'extern "C" uint8_t '+symbol+'_end[];']
 declarations+=['}']
 for asset in assets:
  rel=str(asset.relative_to(src/'assets'));symbol='_binary_'+re.sub(r'[^a-zA-Z0-9_]','_',rel)
  if '/' in rel:raise ValueError('Review nested asset namespaces before adding them')
  declarations+=['inline const IncludedAsset '+re.sub(r'[^a-zA-Z0-9_]','_',rel)+'{Externs::'+symbol+'_start,Externs::'+symbol+'_end};']
 declarations+=['}']
 if name=='airbuds-search':(includes[0]/'assets.hpp').write_text('\n'.join(declarations)+'\n')
 (out/'assets.S').write_text('\n'.join(asm)+'\n')
 mod_id,version,soname=('BetterSongSearch','2.2.9','libBetterSongSearch') if name=='bettersongsearch' else ('airbuds-search','2.0.2','libairbuds-search')
 flags=['-std=c++23','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fvisibility=hidden','-fdeclspec',
        '-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS',
        '-DVERSION="'+version+'"','-DMOD_ID="'+mod_id+'"','-DGIT_COMMIT=0','-DGIT_MODIFIED=1','-DGIT_BRANCH="scotland2-port"','-DGIT_USER="source-port"',
        '-DHOT_RELOAD=0','-DSC2_DIAGNOSTICS=0','-Wno-invalid-offsetof','-Wno-extra-qualification',
        '-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp'),
        '-include',str(ROOT/'ports/scotland2/src/search-port-support.hpp'),*reproducible_flags(ROOT,ndk)]
 flags += [arg for path in includes for arg in ['-I',str(path)]]
 if name=='bettersongsearch':flags.append('-DKALEB_VERSION="0.1.9"')
 key=hashlib.sha256(json.dumps(flags).encode())
 for path in sorted(p for p in src.rglob('*') if p.is_file() and p.suffix!='.cpp'):key.update(path.read_bytes())
 for path in [ROOT/'ports/scotland2/src/search-port-support.hpp',ROOT/'ports/scotland2/bshook-sdk.patch',ROOT/'ports/scotland2/build-inputs.json']:key.update(path.read_bytes())
 stamp=key.hexdigest()
 def one(source):
  obj=objects/(str(source.relative_to(src)).replace('/','__')+'.o');digest=hashlib.sha256(stamp.encode()+source.read_bytes()).hexdigest();receipt=obj.with_suffix('.key')
  if not (obj.exists() and receipt.exists() and receipt.read_text()==digest):
   print('Compiling',source.relative_to(ROOT),flush=True)
   with obj.with_suffix('.log').open('w') as log:result=subprocess.run([str(cxx),*flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
   if result.returncode:
    print(obj.with_suffix('.log').read_text(),file=sys.stderr);raise RuntimeError('Compile failed: '+str(source.relative_to(src)))
   receipt.write_text(digest)
  return obj
 with concurrent.futures.ThreadPoolExecutor(max_workers=max(1,min(4,int(os.environ.get('SC2_JOBS','2'))))) as pool:compiled=list(pool.map(one,sources))
 subprocess.run([str(tools/'aarch64-linux-android26-clang'),'-fPIC','-c',str(out/'assets.S'),'-o',str(out/'assets.o')],check=True)
 shared=[('android-aot','sl2_abi'),('android-customtypes','custom-types'),('android-customtypes/deps','beatsaber-hook'),
         ('android-customtypes/deps','paper2_scotland2'),('android-bsml','bsml'),('android-songcore','songcore'),
         ('android-webutils','web-utils'),('android-song-details','songdetails'),('android-metacore','metacore')]
 # Preserve the original released BeatSaver API already used by PlaylistCore.
 lock=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text());original=next(b for b in lock['binaries'] if b['mod']=='beatsaverplusplus')
 deps=out/'deps';deps.mkdir(exist_ok=True);api=ROOT/'port-results/coremods'/original['file']
 if hashlib.sha256(api.read_bytes()).hexdigest()!=original['sha256']:raise ValueError('BeatSaverPlusPlus input mismatch')
 import shutil
 shutil.copyfile(api,deps/'libbeatsaverplusplus.so')
 libraries=[arg for path,lib in shared for arg in ['-L'+str(ROOT/'build/scotland2'/path),'-l'+lib]]
 libraries+=['-L'+str(deps),'-lbeatsaverplusplus','-llog','-lz','-ldl']
 debug=out/(soname+'.debug.so');binary=out/(soname+'.so')
 subprocess.run([str(cxx),'-shared',*[str(p) for p in compiled],str(out/'assets.o'),*libraries,
                 '-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,'+soname+'.so','-o',str(debug)],check=True)
 subprocess.run([str(tools/'llvm-objcopy'),'--strip-debug',str(debug),str(binary)],check=True)
 subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py','--mod',str(binary),
                 '--game','beatsaber140/lib/arm64-v8a/libil2cpp.so','--output',str(out/'hook-catalog.json')],check=True)
 catalog=json.loads((out/'hook-catalog.json').read_text())
 (out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
 sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf
 elf=Elf(str(binary));imports={n for n,v in elf.imports}
 if not {'setup','late_load'}<=elf.exports or {'mmap','mprotect','popen','EVP_DecodeBlock'}&imports:raise ValueError('Unexpected entry points or platform imports')
 record={'name':mod_id,'version':version,'sourceFilesBuilt':len(sources),'assetsEmbedded':len(assets),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),
         'hookUses':len(catalog['catalog']),'buildKey':stamp,'scope':'Original UI/search/download implementation compiled; no headset launch or authenticated Airbuds call performed'}
 (out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n');print('Built original',mod_id,binary,flush=True)

if __name__=='__main__':main()
