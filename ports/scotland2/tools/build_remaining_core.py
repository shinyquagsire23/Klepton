#!/usr/bin/env python3
"""Build the pinned original remaining core mods against the existing mod SDK."""
import hashlib,json,os,subprocess,sys
from pathlib import Path
ROOT=Path(__file__).resolve().parents[3];os.chdir(ROOT)
name=sys.argv[1];version,modid,soname={'datakeeper':('0.2.5','datakeeper','libdatakeeper'),'fakeid':('0.1.9','FakeId','libFakeId'),'unicode':('0.1.7','unicode','libunicode')}[name]
src=ROOT/'ports/scotland2'/name/'upstream';out=ROOT/'build/scotland2'/('android-'+name);out.mkdir(parents=True,exist_ok=True)
sdk=ROOT/'build/scotland2/mod-sdk';dep=ROOT/'port-deps/sources'
for n,target in [('config-utils',dep/'config-utils'),('autohooks',dep/'autohooks'),('metacore',ROOT/'ports/scotland2/metacore/upstream')]:
 link=sdk/n
 if not link.exists():link.symlink_to(target,target_is_directory=True)
 assert link.resolve()==target.resolve()
ndk=Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'));cc=ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android26-clang++'
il2cpp=dep/'libil2cpp/il2cpp'
includes=[ROOT/'ports/scotland2/src',src,src/'include',sdk,dep/'bs-cordl/include',dep/'fmt/include',dep/'utfcpp/source',il2cpp/'libil2cpp',il2cpp/'external/baselib/Include',il2cpp/'external/baselib/Platforms/Android/Include']
flags=['-std=c++20','-O1','-gline-tables-only','-fPIC','-frtti','-fexceptions','-fdeclspec','-fvisibility=hidden','-DANDROID','-DUNITY_2021','-DHAS_CODEGEN','-DNEED_UNSAFE_CSHARP','-DFMT_HEADER_ONLY','-DSC2_AOT_HOOKS','-DVERSION="'+version+'"','-DMOD_ID="'+modid+'"','-Wno-invalid-offsetof','-Wno-extra-qualification','-include',str(ROOT/'ports/scotland2/src/android-mod-sdk-prelude.hpp')]
if name=='unicode':flags+=['-DSC2_UNICODE_APPLE_GLYPHS']
from build_flags import reproducible_flags
flags += reproducible_flags(ROOT,ndk)
flags.append('-DSC2_DIAGNOSTICS='+('0' if os.environ.get('SC2_BUILD_MODE')=='release' else '1'))
flags += [a for p in includes for a in ['-I',str(p)]]
sources=sorted((src/'src').rglob('*.cpp'))
if name=='datakeeper' and os.environ.get('SC2_BUILD_MODE')!='release':sources.append(ROOT/'ports/scotland2/tests/android-datakeeper-files.cpp')
if name=='unicode' and os.environ.get('SC2_BUILD_MODE')!='release':sources.append(ROOT/'ports/scotland2/tests/android-unicode-raster.cpp')
objects=[]
for source in sources:
 obj=out/(str(source.relative_to(ROOT)).replace('/','_')+'.o');objects.append(obj)
 print('Compiling',source.relative_to(ROOT),flush=True)
 subprocess.run([str(cc),*flags,'-c',str(source),'-o',str(obj)],check=True)
link=[str(cc),'-shared',*[str(o) for o in objects],'-Lbuild/scotland2/android-aot','-lsl2_abi','-Lbuild/scotland2/android-customtypes','-lcustom-types','-Lbuild/scotland2/android-customtypes/deps','-lbeatsaber-hook','-lpaper2_scotland2','-Lbuild/scotland2/android-metacore','-lmetacore','-Lbuild/scotland2/android-bsml','-lbsml','-llog','-Wl,-z,max-page-size=16384','-Wl,--exclude-libs,ALL','-Wl,-soname,'+soname+'.so','-o',str(out/(soname+'.debug.so'))]
subprocess.run(link,check=True)
subprocess.run([str(cc.parent/'llvm-objcopy'),'--strip-debug',str(out/(soname+'.debug.so')),str(out/(soname+'.so'))],check=True)
subprocess.run([sys.executable,'ports/scotland2/tools/read_hook_catalog.py','--mod',str(out/(soname+'.so')),'--game','beatsaber140/lib/arm64-v8a/libil2cpp.so','--output',str(out/'hook-catalog.json')],check=True)
catalog=json.loads((out/'hook-catalog.json').read_text());assert len(catalog['catalog'])=={'datakeeper':4,'fakeid':1,'unicode':4}[name]
(out/'hook-profile.txt').write_text(''.join(t['vaddr']+' '+t['expect']+' '+t['name'].replace(' ','_')+'\n' for t in catalog['targets']))
sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf
elf=Elf(str(out/(soname+'.so')));assert {'setup', 'load' if name=='datakeeper' else 'late_load'}<=elf.exports;imports={x for x,v in elf.imports}
assert 'A64HookFunction' in imports and not {'mmap','mprotect'}&imports
record=json.loads((src.parent/'provenance.json').read_text());record.update(version=version,originalFunctionalityRetained=True,compilerHookCount=len(catalog['catalog']),importsAdaptedAnd64=True,files={str(p.relative_to(ROOT)):hashlib.sha256(p.read_bytes()).hexdigest() for p in [out/(soname+'.so'),out/'hook-catalog.json',src.parent/'upstream.patch']},scope='Original mod source built against adapted shared hook backend; physical verification pending.')
if name=='datakeeper':record['disabledLegacyLightingReset']=True
if name=='unicode':
 record['platformGlyphRasterizer']='Apple CoreText via flat C bridge; original TMP cache/atlas retained'
 record['platformAtlasUpload']='Retained Texture2D.SetPixels32 region API and Apply; stripped Graphics.CopyTexture_Region bypassed'
(out/'provenance.json').write_text(json.dumps(record,indent=2)+'\n');print('Built',name,out/(soname+'.so'))
