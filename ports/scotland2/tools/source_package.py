#!/usr/bin/env python3
"""Create and inspect a deterministic source-only builder archive from Git files."""
import argparse,hashlib,json,pathlib,re,stat,subprocess,zipfile
import bootstrap
ROOT=pathlib.Path(__file__).resolve().parents[3]
PREFIX='beat-saber-custom-songs-builder'
PRIVATE=re.compile(rb'/Users/[A-Za-z0-9_.-]+/|\b0000[0-9A-F]{4}-[0-9A-F]{16}\b|-----BEGIN (?:[A-Z ]*PRIVATE KEY|PKCS12)-----')
FORBIDDEN={'.apk','.obb','.ipa','.mobileprovision','.p12','.pfx','.key','.qmod','.so','.dylib','.a','.log'}
def omitted(name,originals):
 path=pathlib.PurePosixPath(name)
 return name in originals or name.startswith('ports/scotland2/fakeid/') or 'kl_songprobe' in name or 'songprobe' in path.name.lower() or any(part in {'.git','__pycache__','node_modules'} for part in path.parts)
def verify(path):
 with zipfile.ZipFile(path) as z:
  names=z.namelist()
  if len(names)!=len(set(names)):raise ValueError('duplicate archive entries')
  manifest=json.loads(z.read(PREFIX+'/SOURCE-MANIFEST.json'))
  if manifest.get('sourceSnapshotSha256') and hashlib.sha256(json.dumps(manifest['files'],sort_keys=True).encode()).hexdigest()!=manifest['sourceSnapshotSha256']:raise ValueError('source snapshot fingerprint mismatch')
  expected={PREFIX+'/'+p for p in manifest['files']}|{PREFIX+'/SOURCE-MANIFEST.json'}
  if set(names)!=expected:raise ValueError('archive does not match manifest')
  for name in names:
   rel=bootstrap.safe_name(name,0)
   if rel is None or rel.parts[0]!=PREFIX:raise ValueError('invalid archive root')
   info=z.getinfo(name);data=z.read(name)
   if stat.S_ISLNK(info.external_attr>>16):raise ValueError('source archive contains a link')
   if pathlib.Path(name).suffix.lower() in FORBIDDEN:raise ValueError('private/generated file class: '+name)
   if data[:4] in {b'\x7fELF',b'\xcf\xfa\xed\xfe',b'\xca\xfe\xba\xbe'} or PRIVATE.search(data):raise ValueError('private/binary content: '+name)
   if name.endswith('.pem') and b'PRIVATE KEY' in data:raise ValueError('private signing material')
   if name!=PREFIX+'/SOURCE-MANIFEST.json' and hashlib.sha256(data).hexdigest()!=manifest['files'][str(pathlib.PurePosixPath(name).relative_to(PREFIX))]:raise ValueError('archive hash mismatch: '+name)
 return {'archiveSha256':bootstrap.digest(path),'fileCount':len(names),'contentsMatchManifest':True,'privateMaterialScan':'passed checked names, signatures and private identity/key patterns; public CA certificate bundle permitted','scope':'Source package inspection; build and device checks recorded separately'}
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=pathlib.Path);p.add_argument('--verify',type=pathlib.Path);p.add_argument('--local-snapshot',action='store_true',help='Explicitly export audited local changes and declared new builder files; record the base Git revision and modified scope');a=p.parse_args()
 if a.verify:print(json.dumps(verify(a.verify),indent=2));return
 if not a.output:p.error('--output is required')
 output=a.output.resolve()
 if output.exists():raise ValueError('refusing to overwrite source archive')
 if output.is_relative_to(ROOT):raise ValueError('package output must be separate from source')
 lock=json.loads((ROOT/'ports/scotland2/source-inputs.json').read_text())
 dirty=bool(subprocess.check_output(['git','status','--porcelain','--untracked-files=normal'],cwd=ROOT,text=True).strip())
 if dirty and not a.local_snapshot:raise ValueError('Commit source changes before exporting, or explicitly request --local-snapshot')
 declared=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text()).get('builderSources',{})
 originals={str(pathlib.PurePosixPath(row['target'])/name) for row in lock['sources'] for name in row['files']}
 files={}
 for name in sorted(set(subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines())|set(declared)):
  if omitted(name,originals):continue
  source=ROOT/name
  if source.is_symlink():raise ValueError('tracked source link: '+name)
  if name in declared and bootstrap.digest(source)!=declared[name]:raise ValueError('Declared builder source changed: '+name)
  files[name]=(source.read_bytes(),0o755 if source.stat().st_mode&0o111 else 0o644)
 # Preserve available original notices without redistributing the original source trees.
 for name in sorted(originals):
  if pathlib.PurePosixPath(name).name.upper().startswith(('LICENSE','COPYING','NOTICE')):
   source=ROOT/name
   if source.is_file():files['THIRD-PARTY-LICENSES/'+name]=(source.read_bytes(),0o644)
 manifest={'schema':1,'purpose':'Source-only builder; original third-party sources reconstructed from pinned public archives and adaptation patches','base':'0333512150a91a9ac83ceeae6e89c2d9ddc44eb3','files':{n:hashlib.sha256(b).hexdigest() for n,(b,mode) in sorted(files.items())}}
 manifest['gitRevision']=subprocess.check_output(['git','rev-parse','HEAD'],cwd=ROOT,text=True).strip()
 manifest['workingTreeModified']=dirty
 manifest['sourceSnapshotSha256']=hashlib.sha256(json.dumps(manifest['files'],sort_keys=True).encode()).hexdigest()
 files['SOURCE-MANIFEST.json']=(json.dumps(manifest,indent=2,sort_keys=True).encode()+b'\n',0o644)
 output.parent.mkdir(parents=True,exist_ok=True)
 try:
  with zipfile.ZipFile(output,'x',compression=zipfile.ZIP_DEFLATED,compresslevel=9) as z:
   for name,(data,mode) in sorted(files.items()):
    info=zipfile.ZipInfo(PREFIX+'/'+name,date_time=(2026,10,24,0,0,0));info.create_system=3;info.external_attr=(stat.S_IFREG|mode)<<16;info.compress_type=zipfile.ZIP_DEFLATED;z.writestr(info,data)
  report=verify(output)
 except BaseException:
  output.unlink(missing_ok=True);raise
 output.with_suffix(output.suffix+'.sha256').write_text(report['archiveSha256']+'  '+output.name+'\n')
 output.with_suffix(output.suffix+'.inspection.json').write_text(json.dumps(report,indent=2)+'\n');print(json.dumps(report,indent=2))
if __name__=='__main__':main()
