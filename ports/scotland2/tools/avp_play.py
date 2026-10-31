#!/usr/bin/env python3
"""AVP Play's core and search mod build profile; game data stays with its recipe."""
import argparse,json,os,pathlib,re,shutil,subprocess,sys,xml.etree.ElementTree as ET

ROOT=pathlib.Path(__file__).resolve().parents[3]
PROFILE='SCOTLAND2_AVP_PROFILE_V1'

def profile_configuration(root):
 local=root/'.scotland2-avp-local.json'
 base=pathlib.Path(os.environ.get('AVPPLAY_HOME',str(pathlib.Path.home()/'Library/Application Support/AVPPlay')))
 shared=base/'core-mods-build-inputs.json'
 path=local if local.is_file() else shared
 return json.loads(path.read_text()) if path.is_file() else {}

def validate_metadata(root):
 # The runtime derives its OBB filename from this file, not the installed app's
 # Info.plist. Missing metadata silently selects 1.28's versionCode 545.
 metadata=(root/'beatsaber140/apktool.yml').read_text()
 code=re.search(r'^\s*versionCode:\s*[\'"]?(1716|3237)[\'"]?\s*$',metadata,re.M)
 if not code:
  raise ValueError('Core-mod game metadata must identify a supported Beat Saber build (1716 or 3237)')
 manifest=ET.parse(root/'beatsaber140/AndroidManifest.xml').getroot()
 if manifest.tag!='manifest' or manifest.get('package')!='com.beatgames.beatsaber':
  raise ValueError('Core-mod game manifest must identify Beat Saber')
 if code[1]=='3237':
  profile=json.loads((root/'ports/scotland2/port1453/selected-mods.json').read_text())
  if profile['game']['versionCode']!=3237 or profile['runtimeProfile']!='1.45.3':
   raise ValueError('Missing compatible Unity 6.3 core-mod profile')
 return int(code[1])

def install_app(root,device,destination):
 validate_metadata(root)
 subprocess.run(['xcrun','devicectl','device','install','app','--device',device,str(destination),'--timeout','180'],cwd=root,check=True)
 # The normal run.sh performs this pass even when expensive asset staging is
 # skipped. AVP Play stages the APK/OBB separately, but still needs these two
 # files beside assets/. A failed copy must fail the installation job.
 subprocess.run(['bash',str(root/'visionos/stage_assets.sh'),'--meta',device],cwd=root,check=True)
 print('AVP Play core-mod profile and game metadata installed over the requested app identity; no launch or data reset.',flush=True)

def main():
 parser=argparse.ArgumentParser(description=__doc__)
 parser.add_argument('--no-install',action='store_true',help='Build/sign only; explicitly do not claim device installation')
 args=parser.parse_args()
 if os.environ.get('KLEPTON_TARGET')!='beatsaber140':raise ValueError('This core-mod profile supports only the pinned Beat Saber target')
 team=os.environ.get('KLEPTON_TEAM');bundle=os.environ.get('KLEPTON_BUNDLE_ID');device=os.environ.get('KLEPTON_DEVICE')
 if not team or not bundle or (not args.no_install and not device):raise ValueError('AVP Play must supply the signing team, existing app identity and device')
 version=validate_metadata(ROOT)
 if version==3237:
  command=[sys.executable,str(ROOT/'ports/scotland2/tools/avp_play_1453.py')]
  if args.no_install:command.append('--no-install')
  subprocess.run(command,cwd=ROOT,check=True)
  return
 config=profile_configuration(ROOT)
 def location(key,default):
  path=pathlib.Path(config.get(key,default));return (path if path.is_absolute() else ROOT/path).resolve()
 output=location('output',str(ROOT.parent/(ROOT.name+'-coremods-build')))
 cache=location('cache',str(ROOT.parent/'scotland2-download-cache'))
 command=[sys.executable,str(ROOT/'ports/scotland2/tools/build_release.py'),
          '--game',str(ROOT/'beatsaber140'),'--apk',str(ROOT/'beatsaber-1.40.apk'),
          '--graphics',str(ROOT),'--output',str(output),'--cache',str(cache),
          '--team',team,'--bundle-id',bundle,'--presentation-assets',str(ROOT/'visionos/Assets.xcassets'),
          '--display-name',os.environ.get('KLEPTON_DISPLAY_OVERRIDE','Beat Saber (Custom Songs)')]
 if config.get('offline',False):command.append('--offline')
 if config.get('dependencyWorkspace'):command+=['--dependency-workspace',str(location('dependencyWorkspace',''))]
 if output.exists():command.append('--resume')
 print(PROFILE+': original Scotland2/SongCore/SongDownloader; ten core mods plus BetterSongSearch and Airbuds Search; Fake ID excluded',flush=True)
 subprocess.run(command,cwd=ROOT,check=True)
 app=output/'visionos/build/release-dd/Build/Products/Release-xros/Klepton.app'
 # AVP Play reads expiry/controller metadata from its conventional products path.
 products=ROOT/'visionos/build/dd-device-beatsaber140/Build/Products/Debug-xros'
 if not products.resolve().is_relative_to(ROOT/'visionos/build'):raise ValueError('AVP Play products alias an external input')
 products.mkdir(parents=True,exist_ok=True);destination=products/'Klepton.app'
 if destination.is_symlink():raise ValueError('AVP Play app output is a link')
 if destination.exists():shutil.rmtree(destination)
 shutil.copytree(app,destination)
 subprocess.run(['codesign','--verify','--deep','--strict',str(destination)],check=True)
 if args.no_install:
  print('Profile build and signature verified; NOT installed.',flush=True)
  return
 install_app(ROOT,device,destination)

if __name__=='__main__':
 try:main()
 except (ValueError,OSError,subprocess.CalledProcessError) as error:raise SystemExit(str(error))
