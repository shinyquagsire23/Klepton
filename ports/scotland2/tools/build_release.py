#!/usr/bin/env python3
"""Build a standalone custom-song app from explicit owner inputs in a new workspace."""
import argparse,hashlib,json,os,pathlib,plistlib,re,shutil,subprocess,sys,zipfile,shlex
import bootstrap
ROOT=pathlib.Path(__file__).resolve().parents[3]
INSTALLER_DEFAULTS=('KLEPTON_APP_VERSION','KLEPTON_APP_BUILD','KLEPTON_ROOM_GUARD','KLEPTON_GAME_LANGUAGE','KLEPTON_CONTROLLER_MODE','KLEPTON_RENDER_SCALE','KLEPTON_TEST_ON')

def stage_presentation(source,work):
 if source is None:return
 source=source.resolve();work=work.resolve();record={};total=0
 for name in ['AppIcon-beatsaber140.solidimagestack','StartHero-beatsaber140.imageset']:
  folder=source/name
  if folder.is_symlink():raise ValueError('presentation asset folder is a link')
  if not folder.is_dir():
   if name.startswith('AppIcon'):raise ValueError('AVP Play did not prepare the game icon')
   continue
  files=[]
  for path in folder.rglob('*'):
   if path.is_symlink() or not path.resolve().is_relative_to(source):raise ValueError('presentation asset contains a link')
   if path.is_dir():continue
   if not path.is_file() or path.suffix not in {'.json','.png'}:raise ValueError('unexpected presentation asset: '+path.name)
   size=path.stat().st_size;total+=size
   if size>16*1024*1024 or total>32*1024*1024 or len(files)>=64:raise ValueError('presentation assets exceed their bounds')
   if path.suffix=='.json':
    if size>1024*1024:raise ValueError('presentation JSON is too large')
    value=json.loads(path.read_text())
    if not isinstance(value,dict):raise ValueError('presentation JSON must be an object')
    rows=[]
    for key in ['images','layers']:
     entries=value.get(key,[])
     if not isinstance(entries,list) or any(not isinstance(row,dict) for row in entries):raise ValueError('invalid presentation asset entries')
     rows+=entries
    for row in rows:
     filename=row.get('filename')
     if filename is not None and (not isinstance(filename,str) or not filename or pathlib.PurePosixPath(filename).name!=filename or filename in {'.','..'} or '\\' in filename):raise ValueError('unsafe presentation asset reference')
   files.append(path)
  destination=work/'visionos/Assets.xcassets'/name
  if destination.is_symlink() or not destination.resolve().is_relative_to(work):raise ValueError('presentation output aliases an external path')
  if destination.exists():shutil.rmtree(destination)
  shutil.copytree(folder,destination)
  for path in files:
   copied=destination/path.relative_to(folder)
   if bootstrap.digest(copied)!=bootstrap.digest(path):raise ValueError('presentation asset changed while copied')
   record[str(copied.relative_to(work))]=bootstrap.digest(copied)
 (work/'presentation-receipt.json').write_text(json.dumps({'files':record,'scope':'Owner artwork prepared by AVP Play; private build input, never source distribution'},indent=2)+'\n')
def run(work,env,*args):
 print('+',*args,flush=True);subprocess.run([str(x) for x in args],cwd=work,env=env,check=True)
def preflight(a):
 if not __debug__:raise ValueError('Builder checks require Python optimization to be disabled')
 selection=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text())
 for name,expected in selection['game']['files'].items():
  file=a.game/'lib/arm64-v8a'/('libil2cpp.so' if name=='libil2cpp' else 'libunity.so')
  if not file.is_file() or bootstrap.digest(file)!=expected:raise ValueError('Unsupported game input: '+str(file)+'; requires Beat Saber '+selection['game']['version'])
 if not (a.game/'assets/bin/Data/Managed/Metadata/global-metadata.dat').is_file():raise ValueError('Game input lacks IL2CPP metadata')
 if not a.apk.is_file():raise ValueError('Owner APK is required')
 metadata=selection['game']['metadata']
 if bootstrap.digest(a.game/metadata['path'])!=metadata['sha256']:raise ValueError('Unsupported IL2CPP metadata SHA-256')
 try:
  with zipfile.ZipFile(a.apk) as apk:
   expected={'lib/arm64-v8a/libil2cpp.so':selection['game']['files']['libil2cpp'],'lib/arm64-v8a/libunity.so':selection['game']['files']['libunity'],metadata['path']:metadata['sha256']}
   for name,sha in expected.items():
    h=hashlib.sha256()
    with apk.open(name) as file:
     for chunk in iter(lambda:file.read(1024*1024),b''):h.update(chunk)
    if h.hexdigest()!=sha:raise ValueError('APK differs from extracted game: '+name)
 except (zipfile.BadZipFile,KeyError) as e:raise ValueError('APK lacks matching game payload: '+str(e))
 if not re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9.-]+',a.bundle_id) or '.' not in a.bundle_id:raise ValueError('Invalid bundle ID')
 if not re.fullmatch(r'[A-Z0-9]{10}',a.team):raise ValueError('Signing team must be an explicit 10-character team ID')
 for row in json.loads((ROOT/'ports/scotland2/graphics-inputs.json').read_text())['files']:
  path=a.graphics/row['path']
  if not path.is_file() or bootstrap.digest(path)!=row['sha256']:raise ValueError('Graphics input differs from the declared cache: '+row['path'])
 if a.output==ROOT or ROOT.is_relative_to(a.output) or a.output.is_relative_to(ROOT):raise ValueError('Output must be separate from the canonical source tree')

def source_files():
 manifest=ROOT/'SOURCE-MANIFEST.json'
 if manifest.exists():return json.loads(manifest.read_text())['files']
 files={p:bootstrap.digest(ROOT/p) for p in subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines() if (ROOT/p).is_file()}
 for name,expected in json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text()).get('builderSources',{}).items():
  if bootstrap.safe_name(name,0) is None or (ROOT/name).is_symlink() or bootstrap.digest(ROOT/name)!=expected:raise ValueError('Declared builder source changed: '+name)
  files[name]=expected
 return files
def stage_source(work):
 files=source_files()
 prior=work/'source-receipt.json'
 if prior.exists():
  for name in set(json.loads(prior.read_text())['files'])-set(files):
   rel=pathlib.PurePosixPath(name)
   if rel.is_absolute() or '..' in rel.parts:raise ValueError('unsafe prior source receipt')
   old=work/name
   if not old.resolve().is_relative_to(work.resolve()):raise ValueError('prior source points outside workspace')
   old.unlink(missing_ok=True)
 for name,expected in files.items():
  rel=pathlib.PurePosixPath(name)
  if rel.is_absolute() or '..' in rel.parts:raise ValueError('unsafe source manifest path')
  source=ROOT/name
  if source.is_symlink() or bootstrap.digest(source)!=expected:raise ValueError('source manifest mismatch: '+name)
  dest=work/name
  if not dest.resolve().is_relative_to(work.resolve()):raise ValueError('source destination aliases an external input: '+name)
  dest.parent.mkdir(parents=True,exist_ok=True);shutil.copy2(source,dest)
 (work/'source-receipt.json').write_text(json.dumps({'files':files},sort_keys=True)+'\n')

def validate(app,bundle_id):
 selection=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text());meta=app/'Scotland2Metadata';info=plistlib.loads((app/'Info.plist').read_bytes())
 if info['CFBundleIdentifier']!=bundle_id:raise ValueError('app bundle ID differs from requested update identity')
 if str(info.get('KLScotland2Diagnostics'))!='0' or str(info.get('KLTestBuild'))!='0' or str(info.get('KLAllowTestSwitches'))!='0':raise ValueError('diagnostic/test controls enabled in release')
 if str(info.get('KLStartScreen'))!='1':raise ValueError('release start/settings screen is disabled')
 actual={str(p.relative_to(meta)) for p in meta.rglob('*.so')}
 if actual!=set(selection['metadata']):raise ValueError('stale or missing mod metadata: '+str(actual^set(selection['metadata'])))
 for relative in selection['metadata']:
  name=pathlib.Path(relative).stem
  if not (app/'Frameworks'/(name+'.framework')/name).is_file():raise ValueError('missing embedded signed framework: '+name)
 if b'CustomTypesMallocInfoOnExit.xml' in (meta/'libs/libcustom-types.so').read_bytes():raise ValueError('custom-types GC tracing remains in release')
 if any('fakeid' in str(p.relative_to(app)).lower() or '_probe' in p.name for p in app.rglob('*')):raise ValueError('excluded/diagnostic component in app')
 sys.path.insert(0,str(ROOT/'tools'));from audit_elf import Elf
 for p in meta.rglob('*.so'):
  if any(name.startswith('sc2_test_') for name in Elf(str(p)).exports):raise ValueError('diagnostic export in release module: '+p.name)
 return {'bundleId':bundle_id,'selectionSha256':bootstrap.digest(ROOT/'ports/scotland2/selected-mods.json'),'metadata':sorted(actual),'releaseControlsDisabled':True,'gcTracingAbsent':True,'fakeIdAbsent':True,'startSettingsScreenEnabled':True,'iconName':info.get('CFBundleIconName'),'scope':'Package structure only; device checks are separate'}

def main():
 p=argparse.ArgumentParser(description=__doc__)
 for name in ['game','apk','graphics','output','cache']:p.add_argument('--'+name,type=pathlib.Path,required=True)
 p.add_argument('--presentation-assets',type=pathlib.Path,help='Private icon/start artwork prepared by AVP Play')
 p.add_argument('--resume',action='store_true',help='Resume only an owned builder workspace; record a new source snapshot');p.add_argument('--team',required=True);p.add_argument('--bundle-id',required=True);p.add_argument('--display-name',default='Beat Saber (Custom Songs)');p.add_argument('--seed',type=pathlib.Path,action='append',default=[]);p.add_argument('--offline',action='store_true');p.add_argument('--dependency-workspace',type=pathlib.Path);p.add_argument('--preflight',action='store_true');p.add_argument('--unsigned',action='store_true',help='Build/package without registering or installing an app ID')
 a=p.parse_args()
 for name in ['game','apk','graphics','output','cache']:setattr(a,name,getattr(a,name).resolve())
 preflight(a)
 if a.preflight:print('PASS preflight: exact game, graphics inputs, explicit signing identity and separate output');return
 work=a.output
 resume=work.exists()
 if resume and (not a.resume or not (work/'.sc2-builder-workspace').is_file()):raise ValueError('Output already exists and is not an explicitly resumed builder workspace: '+str(work))
 if resume:
  config=json.loads((work/'builder-settings.json').read_text())
  if config!={'bundleId':a.bundle_id,'team':a.team,'game':str(a.game),'apk':str(a.apk),'graphics':str(a.graphics)}:raise ValueError('Resume settings differ; use a new output')
 else:
  work.mkdir(parents=True);(work/'.sc2-builder-workspace').write_text('Generated private build workspace; never distribute.\n')
  (work/'builder-settings.json').write_text(json.dumps({'bundleId':a.bundle_id,'team':a.team,'game':str(a.game),'apk':str(a.apk),'graphics':str(a.graphics)})+'\n')
 stage_source(work)
 if a.dependency_workspace:
  dep=a.dependency_workspace.resolve();receipt=json.loads((dep/'build-input-receipt.json').read_text());lock=json.loads((ROOT/'ports/scotland2/build-inputs.json').read_text())
  if receipt['manifestSha256']!=bootstrap.digest(ROOT/'ports/scotland2/build-inputs.json') or receipt['dependencyTree']!=bootstrap.tree_hashes(dep/'port-deps/sources'):raise ValueError('declared dependency extraction changed')
  bootstrap.verify_toolchain(dep/'port-deps/toolchains/android-ndk-r27c',lock['ndk']['sha256'])
  for source in (dep/'port-deps/sources').iterdir():
   dest=work/'port-deps/sources'/source.name
   if dest.exists():
    if source.is_file() and bootstrap.digest(source)!=bootstrap.digest(dest):raise ValueError('dependency lock mismatch: '+source.name)
    if source.is_dir():
     for child in source.iterdir():
      target=dest/child.name
      if not target.exists():target.symlink_to(child,target_is_directory=child.is_dir())
   else:dest.symlink_to(source,target_is_directory=source.is_dir())
  for name in ['elf','archives']:
   source=dep/'port-results/coremods'/name
   target=work/'port-results/coremods'/name
   if source.exists() and not target.exists():target.symlink_to(source,target_is_directory=True)
  target=work/'port-deps/toolchains/android-ndk-r27c'
  if not target.exists():target.symlink_to(dep/'port-deps/toolchains/android-ndk-r27c',target_is_directory=True)
  if target.resolve()!=(dep/'port-deps/toolchains/android-ndk-r27c').resolve():raise ValueError('resumed toolchain input changed')
  shutil.copyfile(dep/'build-input-receipt.json',work/'build-input-receipt.json')
 elif not resume:bootstrap.bootstrap(work,a.cache,[x.resolve() for x in a.seed],a.offline)
 bootstrap.materialize_sources(work,a.cache,[x.resolve() for x in a.seed],a.offline)
 if not resume:(work/'beatsaber140').symlink_to(a.game,target_is_directory=True);(work/'beatsaber-1.40.apk').symlink_to(a.apk)
 for name in ([] if resume else ['vendor','vendor-moltenvk']):(work/name).symlink_to(a.graphics/name,target_is_directory=True)
 stage_presentation(a.presentation_assets,work)
 env={k:v for k,v in os.environ.items() if not k.startswith(('KLEPTON_','SC2_','KL_'))}
 env.update({k:os.environ[k] for k in INSTALLER_DEFAULTS if k in os.environ})
 env.pop('PYTHONOPTIMIZE',None)
 env['REPRO_FLAGS']=shlex.join(['-ffile-prefix-map='+str(work)+'=.','-fmacro-prefix-map='+str(work)+'=.','-fdebug-compilation-dir=.'])
 env.update(KLEPTON_TARGET='beatsaber140',KLEPTON_SCOTLAND2='1',SC2_BUILD_MODE='release',SC2_TEST_SET='coremods',XROS_PLATFORMS='xros',KLEPTON_EVA='0',KLEPTON_LOW_LATENCY='0',KLEPTON_TEAM=a.team,KLEPTON_BUNDLE_ID=a.bundle_id,KLEPTON_DISPLAY_OVERRIDE=a.display_name,KL_GUEST_JIT='0')
 env['KLEPTON_START_SCREEN']='1'
 for folder in ['build','visionos/build','visionos/Frameworks','visionos/Scotland2Metadata']:
  if not (work/folder).resolve().is_relative_to(work):raise ValueError('generated output aliases an external path: '+folder)
 run(work,env,'make','build/klepton-ld','xros','XROS_PLATFORMS=xros')
 run(work,env,sys.executable,'ports/scotland2/tools/package_release.py')
 for script in ['mkguest.sh','mkangle.sh','mkmvk.sh']:run(work/'visionos',env,'bash',script)
 run(work,env,'bash','ports/scotland2/prepare-game-hooks.sh')
 run(work/'visionos',env,sys.executable,'gen_xcodeproj.py')
 args=['xcodebuild','-project','Klepton.xcodeproj','-scheme','Klepton','-configuration','Release','-destination','generic/platform=visionOS','-derivedDataPath','build/release-dd']
 args+=['CODE_SIGNING_ALLOWED=NO'] if a.unsigned else ['-allowProvisioningUpdates']
 run(work/'visionos',env,*args,'build')
 app=work/'visionos/build/release-dd/Build/Products/Release-xros/Klepton.app'
 report=validate(app,a.bundle_id)
 if a.presentation_assets and report['iconName']=='AppIcon':raise ValueError('AVP Play icon was not compiled into the app')
 if not a.unsigned:run(work,env,'codesign','--verify','--deep','--strict',app)
 report.update(signed=not a.unsigned,files={str(p.relative_to(app)):bootstrap.digest(p) for p in app.rglob('*') if p.is_file()})
 (work/'build-validation.json').write_text(json.dumps(report,indent=2)+'\n')
 print('Built candidate (not installed):',app)
if __name__=='__main__':
 try:main()
 except (ValueError,OSError,subprocess.CalledProcessError) as e:raise SystemExit(str(e))
