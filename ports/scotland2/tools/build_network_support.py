#!/usr/bin/env python3
"""Rebuild the actual pinned supporting libraries with audited network boundaries."""
import concurrent.futures,hashlib,json,os,pathlib,re,subprocess,sys
from build_flags import reproducible_flags
ROOT=pathlib.Path(__file__).resolve().parents[3];os.chdir(ROOT)
DEP=ROOT/'port-deps/sources';OUT=ROOT/'build/scotland2';SDK=OUT/'mod-sdk'
NDK=pathlib.Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'));BIN=NDK/'toolchains/llvm/prebuilt/darwin-x86_64/bin';CXX=BIN/'aarch64-linux-android26-clang++';CC=BIN/'aarch64-linux-android26-clang'
JOBS=max(1,min(4,int(os.environ.get('SC2_JOBS','2'))))
def run(args):subprocess.run([str(x) for x in args],check=True)
def compile_many(sources,folder,flags,c=False):
 flags=[*flags,*reproducible_flags(ROOT,NDK)]
 folder.mkdir(parents=True,exist_ok=True);stamp_hash=hashlib.sha256(json.dumps(flags).encode()+(ROOT/'ports/scotland2/build-inputs.json').read_bytes())
 for header in sorted((ROOT/'ports/scotland2/song-details/upstream').rglob('*.hpp')):stamp_hash.update(header.read_bytes())
 for header in sorted((ROOT/'ports/scotland2/song-details/upstream').rglob('*.h')):stamp_hash.update(header.read_bytes())
 stamp=stamp_hash.hexdigest()
 def one(source):
  obj=folder/(str(source.relative_to(ROOT)).replace('/','_')+'.o');key=hashlib.sha256(stamp.encode()+source.read_bytes()).hexdigest();receipt=obj.with_suffix('.key')
  if obj.exists() and receipt.exists() and receipt.read_text()==key and os.environ.get('SC2_REBUILD')!='1':return obj
  print('Compiling',source.relative_to(ROOT),flush=True)
  with obj.with_suffix('.log').open('w') as log:r=subprocess.run([str(CC if c else CXX),*flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
  if r.returncode:print(obj.with_suffix('.log').read_text(),file=sys.stderr);raise RuntimeError('Compilation failed: '+str(source))
  receipt.write_text(key);return obj
 with concurrent.futures.ThreadPoolExecutor(max_workers=JOBS) as pool:return list(pool.map(one,sources))
def static_deps():
 protobuf=DEP/'protobuf';sources=[]
 for name in ['libprotobuf-lite.cmake','libprotobuf.cmake']:
  text=(protobuf/'cmake'/name).read_text().split(')\n',1)[0]
  sources += [protobuf/p for p in re.findall(r'\$\{protobuf_SOURCE_DIR\}/([^\s]+\.cc)',text)]
 if len(set(sources))!=len(sources) or len(sources)<50:raise ValueError('unexpected pinned protobuf source lists')
 flags=['-std=c++17','-O1','-fPIC','-fvisibility=hidden','-DHAVE_PTHREAD=1','-DHAVE_ZLIB=1','-I'+str(protobuf/'src')]
 objects=compile_many(sources,OUT/'android-network-static/protobuf',flags)
 ar=OUT/'android-network-static/libprotobuf.a';run([BIN/'llvm-ar','rcs',ar,*objects])
 brotli=DEP/'brotli';sources=sorted((brotli/'c/common').glob('*.c'))+sorted((brotli/'c/dec').glob('*.c'))
 objects=compile_many(sources,OUT/'android-network-static/brotli',['-std=c99','-O1','-fPIC','-fvisibility=hidden','-I'+str(brotli/'c/include')],c=True)
 run([BIN/'llvm-ar','rcs',OUT/'android-network-static/libbrotlidec.a',*objects])

def library(name):
 if name!='song-details':raise ValueError('Only the original SongDetails library is rebuilt; BeatSaverPlusPlus retains its released binary and prepared extraction boundary')
 version,soname='1.0.4','libsongdetails'
 src=ROOT/'ports/scotland2'/name/'upstream';out=OUT/('android-'+name);out.mkdir(parents=True,exist_ok=True)
 for key,target in [('libcurl',DEP/'libcurl-7.78.0'),('web-utils',ROOT/'ports/scotland2/web-utils/upstream')]:
  link=SDK/key
  if not link.exists():link.symlink_to(target,target_is_directory=True)
 # Keep this library's original curl 7.78 headers distinct from WebUtils 8.5.
 local=out/'include';local.mkdir(exist_ok=True);link=local/'libcurl'
 if not link.exists():link.symlink_to(DEP/'libcurl-7.78.0',target_is_directory=True)
 includes=[local,src,src/'include',src/'shared',src/'src',SDK,DEP/'fmt/include',DEP/'rapidjson/include',DEP/'protobuf/src',DEP/'brotli/c/include',DEP/'libil2cpp/il2cpp/libil2cpp',DEP/'libil2cpp/il2cpp/external/baselib/Include',DEP/'libil2cpp/il2cpp/external/baselib/Platforms/Android/Include']
 flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-fvisibility=hidden','-fvisibility-inlines-hidden','-DANDROID','-fdeclspec','-DUNITY_2021','-DNEED_UNSAFE_CSHARP','-DWEBUTILS_NO_BSML','-DFMT_HEADER_ONLY','-DMOD_ID="'+name+'"','-DVERSION="'+version+'"','-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'),'-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
 flags += [item for path in includes for item in ['-I',str(path)]]
 sources=sorted((src/'src').rglob('*.cpp'))+sorted((src/'src').rglob('*.cc'))
 static_deps()
 if os.environ.get('SC2_BUILD_MODE')!='release':sources.append(ROOT/'ports/scotland2/tests'/('android-'+name+'-network.cpp'))
 objects=compile_many(sources,out/'objects',flags)
 libraries=['-L'+str(OUT/'android-customtypes/deps'),'-lpaper2_scotland2']
 libraries += [str(DEP/'libcurl-7.78.0.a'),str(OUT/'android-network-static/libprotobuf.a'),str(OUT/'android-network-static/libbrotlidec.a')]
 debug=out/(soname+'.debug.so');binary=out/(soname+'.so')
 run([CXX,'-shared',*objects,*libraries,'-llog','-lz','-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,'+soname+'.so','-o',debug])
 run([BIN/'llvm-objcopy','--strip-debug',debug,binary])
 sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf
 original=Elf(str(ROOT/'port-results/coremods/elf'/name/version/(soname+'.so')));built=Elf(str(binary))
 required={n for n in original.exports if n.startswith(('_ZN16SongDetailsCache','_ZNK16SongDetailsCache'))}
 missing=sorted(required-built.exports)
 if missing:
  (out/'missing-exports.json').write_text(json.dumps(missing,indent=2)+'\n')
  raise ValueError(str(len(missing))+' original public exports missing; see '+str(out/'missing-exports.json'))
 (out/'provenance.json').write_text(json.dumps({'version':version,'originalPublicExportsPreserved':len(required),'sha256':hashlib.sha256(binary.read_bytes()).hexdigest(),'patchSha256':hashlib.sha256((src.parent/'upstream.patch').read_bytes()).hexdigest(),'scope':'Original library rebuilt with network/extraction boundary fixes; runtime validation is separate'},indent=2)+'\n')
 print('Built original',name,binary,flush=True)
if __name__=='__main__':library(sys.argv[1])
