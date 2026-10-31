#!/usr/bin/env python3
"""Build the authoritative core/search composition into fresh generated directories."""
import hashlib,json,os,pathlib,plistlib,shutil,subprocess,sys
from selection import validate_selection, SEARCH_MODS
ROOT=pathlib.Path(__file__).resolve().parents[3]
def run(*args,**kwargs):subprocess.run([str(a) for a in args],cwd=ROOT,check=True,**kwargs)
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def remove_generated(path):
 if not path.resolve().is_relative_to(ROOT):raise ValueError('generated output aliases an external path: '+str(path))
 shutil.rmtree(path)
def main():
 os.chdir(ROOT)
 if os.environ.get('SC2_BUILD_MODE')!='release':raise SystemExit('Set SC2_BUILD_MODE=release; diagnostic compositions use package.sh')
 selection=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text());inventory=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
 chosen=validate_selection(selection)
 # Input validation happens before any expensive compiler invocation.
 for name,expected in selection['game']['files'].items():
  p=ROOT/'beatsaber140/lib/arm64-v8a'/('libil2cpp.so' if name=='libil2cpp' else 'libunity.so')
  if sha(p)!=expected:raise SystemExit('Unsupported game: '+p.name+' SHA-256 mismatch')
 run('make','build/klepton-ld')
 run('bash','ports/scotland2/build.sh','xros')
 env=dict(os.environ,SC2_ANDROID_AOT='1',SC2_ANDROID_CAPSTONE='1');run('bash','ports/scotland2/build-android-paths.sh',env=env)
 run('bash','ports/scotland2/build-flamingo.sh')
 run('bash','ports/scotland2/build-customtypes.sh')
 for name in ['lapiz','bsml','songcore','webutils','metacore','playlistcore','songdownloader']:
  run('bash','ports/scotland2/build-'+name+'.sh')
  if name=='webutils':
   for library in ['song-details']:run(sys.executable,'ports/scotland2/tools/build_network_support.py',library)
 for name in ['datakeeper','unicode']:run(sys.executable,'ports/scotland2/tools/build_remaining_core.py',name)
 if SEARCH_MODS<=chosen:
  for name in ['bettersongsearch','airbuds-search']:run(sys.executable,'ports/scotland2/tools/build_search_mods.py',name)
 bundle=ROOT/'build/scotland2/xros-release/bundle'
 # Generated output only, rooted under this builder workspace.
 if bundle.exists():remove_generated(bundle)
 meta=bundle/'Scotland2Metadata';frameworks=bundle/'Frameworks'
 for phase in ['libs','early_mods','mods','shims','profiles']: (meta/phase).mkdir(parents=True)
 frameworks.mkdir()
 built={'libsongdetails':'android-song-details','libcustom-types':'android-customtypes','liblapiz':'android-lapiz','libbsml':'android-bsml','libsongcore':'android-songcore','libweb-utils':'android-webutils','libmetacore':'android-metacore','libplaylistcore':'android-playlistcore','libsongdownloader':'android-songdownloader','libdatakeeper':'android-datakeeper','libunicode':'android-unicode','libsl2_abi':'android-aot','libsc2_flamingo':'android-flamingo','libsc2_scene':'android-flamingo'}
 built.update({'libBetterSongSearch':'android-bettersongsearch','libairbuds-search':'android-airbuds-search'})
 for relative in selection['metadata']:
  dest=meta/relative;name=dest.stem
  if name in built:source=ROOT/'build/scotland2'/built[name]/(name+'.so')
  else:
   matches=[b for b in inventory['binaries'] if pathlib.Path(b['file']).name==dest.name and b['mod']!='FakeId']
   if len(matches)!=1:raise ValueError('ambiguous library '+name)
   row=matches[0];source=ROOT/'port-results/coremods'/row['file']
   if sha(source)!=row['sha256']:raise ValueError('original library hash mismatch '+name)
  ndk=pathlib.Path(os.environ.get('ANDROID_NDK_HOME',ROOT/'port-deps/toolchains/android-ndk-r27c'))
  run(ndk/'toolchains/llvm/prebuilt/darwin-x86_64/bin/llvm-objcopy','--strip-debug',source,dest)
  run('bash','ports/scotland2/stage-mod-framework.sh','xros',dest,frameworks)
 # Android C++ runtime has a filesystem-safe Apple framework name.
 source=ROOT/'build/scotland2/android-aot/libc++_shared.so'
 fw=frameworks/'libcxx_shared.framework';fw.mkdir()
 run(ROOT/'build/klepton-ld',source,'--platform','visionos','--install-name','@rpath/libcxx_shared.framework/libcxx_shared','-o',fw/'libcxx_shared','--quiet')
 info={'CFBundleExecutable':'libcxx_shared','CFBundleIdentifier':'dev.klepton.scotland2.libcxx-shared','CFBundleName':'libcxx_shared','CFBundlePackageType':'FMWK','CFBundleShortVersionString':'1.0','CFBundleVersion':'1','MinimumOSVersion':'26.0'};(fw/'Info.plist').write_bytes(plistlib.dumps(info))
 core=frameworks/'Scotland2Core.framework';core.mkdir();sdk=subprocess.check_output(['xcrun','--sdk','xros','--show-sdk-path'],text=True).strip()
 run('xcrun','clang++','-target','arm64-apple-xros26.0','-isysroot',sdk,'-dynamiclib','-Wl,-force_load,'+str(ROOT/'build/scotland2/xros/libscotland2-core.a'),'-install_name','@rpath/Scotland2Core.framework/Scotland2Core','-o',core/'Scotland2Core')
 info.update(CFBundleExecutable='Scotland2Core',CFBundleName='Scotland2Core',CFBundleIdentifier='dev.klepton.scotland2.core');(core/'Info.plist').write_bytes(plistlib.dumps(info))
 # Prepare the narrow replacements for upstream filesystem boundaries.
 boundaries=[('libbeatsaber-hook','_Z6mkpathNSt6__ndk117basic_string_viewIcNS_11char_traitsIcEEEE'),
             ('libbeatsaverplusplus','_ZN9BeatSaver5Utils10ExtractAllENSt6__ndk14spanIKhLm18446744073709551615EEERKNS1_4__fs10filesystem4pathE')]
 for name,symbol in boundaries:
  fw=frameworks/(name+'.framework');source=meta/'libs'/(name+'.so');unprepared=fw/'unprepared.dylib';manifest=fw/'manifest.json'
  run(ROOT/'build/klepton-ld',source,'--platform','visionos','--install-name','@rpath/'+name+'.framework/'+name,'-o',unprepared,'--quiet')
  run(sys.executable,'ports/scotland2/tools/hook_manifest.py','--elf',source,'--translation',unprepared,'--output',manifest,symbol)
  run(sys.executable,'ports/scotland2/tools/prepare_hooks.py','--elf',source,'--input',unprepared,'--manifest',manifest,'--output',fw/name)
  unprepared.unlink();manifest.unlink();(fw/(name+'.hooks.json')).unlink(missing_ok=True)
 for mod in selection['hookCatalogs']:shutil.copyfile(ROOT/'build/scotland2'/('android-'+mod)/'hook-profile.txt',meta/'profiles'/(mod+'-hooks.txt'))
 cert=ROOT/'ports/scotland2/certificates';trust=json.loads((cert/'provenance.json').read_text())
 if sha(cert/'cacert.pem')!=trust['sha256']:raise ValueError('certificate bundle mismatch')
 shutil.copytree(cert,meta/'certificates')
 record={'schema':1,'selection':selection['name'],'selectionSha256':sha(ROOT/'ports/scotland2/selected-mods.json'),'mode':'release','excludedMods':selection['excludedMods'],'libraries':{str(p.relative_to(meta)):sha(p) for p in meta.rglob('*.so')},'scope':'Built candidate; runtime validation is recorded separately.'}
 (meta/'provenance.json').write_text(json.dumps(record,indent=2)+'\n')
 output=ROOT/'visionos/Frameworks';output.mkdir(parents=True,exist_ok=True)
 for p in output.glob('SC2_*.xcframework'):remove_generated(p)
 for framework in sorted(frameworks.glob('*.framework')):
  name=framework.stem;dest=output/(('Scotland2Core' if name=='Scotland2Core' else 'SC2_'+name)+'.xcframework')
  if dest.exists():remove_generated(dest)
  run('xcodebuild','-create-xcframework','-framework',framework,'-output',dest)
 dest=ROOT/'visionos/Scotland2Metadata'
 if dest.exists():remove_generated(dest)
 shutil.copytree(meta,dest)
 print('Release composition packaged:',bundle,flush=True)
if __name__=='__main__':main()
