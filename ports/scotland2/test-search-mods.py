#!/usr/bin/env python3
"""Load the original search ports with real IL2CPP in an isolated Mac harness.

Requires the existing prepared BSML Mac test bundle and compiled search ports.
This tests constructors, setup and strict imports; never calls late_load, logs
into Airbuds, installs an app, or uses the headset's saves.
"""
import os,pathlib,shutil,subprocess
ROOT=pathlib.Path(__file__).resolve().parents[2]
def main():
 os.chdir(ROOT)
 base=ROOT/'build/scotland2/search-load-host';base.mkdir(exist_ok=True)
 bundle=base/'bundle'
 if bundle.is_symlink() or not bundle.resolve().is_relative_to(base):raise ValueError('Unsafe host output')
 if not bundle.exists():subprocess.run(['cp','-cR','build/scotland2/macosx-bsml/bundle',str(bundle)],check=True)
 meta=bundle/'Scotland2Metadata';fw=bundle/'Frameworks'
 subprocess.run(['make','build/t_scotland2_il2cpp','build/klepton-ld'],check=True)
 for folder,soname in [('bettersongsearch','libBetterSongSearch'),('airbuds-search','libairbuds-search')]:
  src=ROOT/('build/scotland2/android-'+folder)/(soname+'.so')
  shutil.copy2(src,meta/'test-only'/src.name)
  subprocess.run(['./build/klepton-ld',str(src),'--platform','macos','-o',str(fw/(soname+'.dylib')),'--quiet'],check=True)
  subprocess.run(['codesign','--force','--sign','-',str(fw/(soname+'.dylib'))],check=True)
 env={k:v for k,v in os.environ.items() if not k.startswith(('SC2_','KL_'))}
 env.update(SC2_LAPIZ_CATALOG=str(ROOT/'build/scotland2/android-lapiz/hook-catalog.json'),SC2_BSML_CATALOG=str(ROOT/'build/scotland2/android-bsml/hook-catalog.json'))
 subprocess.run(['bash','ports/scotland2/prepare-customtypes-host.sh',str(fw)],check=True,env=env)
 env.pop('SC2_BSML_CATALOG')
 for soname,mid,version in [('libBetterSongSearch','BetterSongSearch','2.2.9'),('libairbuds-search','airbuds-search','2.0.2')]:
  userdata=base/('userdata-'+mid);userdata.mkdir(exist_ok=True)
  runenv=dict(env,KL_SCOTLAND2='1',KL_DYLIB_DIR=str(fw),KL_FILES_DIR=str(userdata),KL_SCOTLAND2_NATIVE_GATES='1',KL_SCOTLAND2_ABI='1',KL_SCOTLAND2_FILESYSTEM='1',KL_SCOTLAND2_BSML_PROBE='1',KL_SCOTLAND2_LAPIZ_PROBE='0',KL_GUEST_JIT='0',KL_SCOTLAND2_CUSTOMTYPES_UNITY=str(ROOT/'beatsaber140/lib/arm64-v8a/libunity.so'),SC2_SEARCH_MODULE=str(meta/'test-only'/(soname+'.so')),SC2_SEARCH_SONGCORE=str(meta/'test-only/libsongcore.so'),SC2_SEARCH_ID=mid,SC2_SEARCH_VERSION=version)
  with (base/(mid+'.log')).open('w') as log:
   subprocess.run(['./build/t_scotland2_il2cpp',str(ROOT/'beatsaber140/lib/arm64-v8a/libil2cpp.so'),str(ROOT/'beatsaber140/assets/bin/Data/Managed')],env=runenv,stdout=log,stderr=subprocess.STDOUT,check=True)
  print('PASS original load/setup/imports:',mid,version,flush=True)
if __name__=='__main__':main()
