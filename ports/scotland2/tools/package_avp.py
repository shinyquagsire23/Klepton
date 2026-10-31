#!/usr/bin/env python3
"""Package the audited source archive and hash-pinned public graphics for AVP Play."""
import argparse,datetime,gzip,hashlib,io,json,pathlib,subprocess,tarfile,zipfile
import bootstrap,source_package
ROOT=pathlib.Path(__file__).resolve().parents[3]

def main():
 p=argparse.ArgumentParser(description=__doc__)
 p.add_argument('--source-archive',type=pathlib.Path,required=True)
 p.add_argument('--graphics',type=pathlib.Path,required=True)
 p.add_argument('--output',type=pathlib.Path,required=True)
 p.add_argument('--local-snapshot',action='store_true',help='Package an explicitly marked source snapshot without claiming a clean Git revision')
 a=p.parse_args()
 source_package.verify(a.source_archive)
 def git(*args):return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()
 dirty=bool(git('status','--porcelain','--untracked-files=normal'))
 if dirty and not a.local_snapshot:raise ValueError('Commit source changes before packaging, or explicitly request --local-snapshot')
 revision=git('rev-parse','HEAD');timestamp=int(git('show','-s','--format=%ct','HEAD'))
 files={}
 with zipfile.ZipFile(a.source_archive) as z:
  for item in z.infolist():
   name=str(pathlib.PurePosixPath(item.filename).relative_to(source_package.PREFIX))
   files[name]=(z.read(item),0o755 if (item.external_attr>>16)&0o111 else 0o644)
 # Tie the source archive to the exact working tree used to identify this package.
 manifest=json.loads(files['SOURCE-MANIFEST.json'][0])
 if manifest.get('gitRevision')!=revision:raise ValueError('Source archive Git base differs from current checkout')
 if bool(manifest.get('workingTreeModified'))!=dirty:raise ValueError('Source snapshot does not describe the current Git scope')
 for name,expected in manifest['files'].items():
  if name.startswith('THIRD-PARTY-LICENSES/'):continue
  if bootstrap.digest(ROOT/name)!=expected:raise ValueError('Source archive is stale: '+name)
 for row in json.loads(files['ports/scotland2/graphics-inputs.json'][0])['files']:
  path=a.graphics/row['path'];data=path.read_bytes()
  if hashlib.sha256(data).hexdigest()!=row['sha256']:raise ValueError('Graphics hash mismatch: '+row['path'])
  files[row['path']]=(data,0o644)
 metadata={'format':1,'version':int(git('rev-list','--count','HEAD')),'commit':revision[:7],
           'ancestors':git('rev-list','--abbrev-commit','--abbrev=7','HEAD').splitlines(),
           'created':datetime.datetime.fromtimestamp(timestamp,datetime.timezone.utc).isoformat().replace('+00:00','Z'),
           'modified':True}
 metadata['sourceSnapshotSha256']=manifest.get('sourceSnapshotSha256')
 metadata['workingTreeModified']=dirty
 files['toolchain.json']=(json.dumps(metadata,indent=2,sort_keys=True).encode()+b'\n',0o644)
 if 'ports/scotland2/tools/avp_play.py' not in files:raise ValueError('Missing AVP adapter')
 output=a.output.resolve();output.parent.mkdir(parents=True,exist_ok=True)
 if output.exists():raise ValueError('Refusing to overwrite an existing package')
 with output.open('xb') as raw,gzip.GzipFile(filename='',mode='wb',fileobj=raw,mtime=0) as compressed,tarfile.open(fileobj=compressed,mode='w',format=tarfile.USTAR_FORMAT) as tar:
  for name,(data,mode) in sorted(files.items()):
   entry=tarfile.TarInfo(name);entry.size=len(data);entry.mode=mode;entry.mtime=timestamp
   tar.addfile(entry,io.BytesIO(data))
 metadata.update(archive=output.name,sha256=bootstrap.digest(output),size=output.stat().st_size)
 output.with_name(output.name+'.json').write_text(json.dumps(metadata,indent=2,sort_keys=True)+'\n')
 print(json.dumps({'sha256':metadata['sha256'],'files':len(files),'sourceCommit':revision,'gameOrSigningMaterial':False},indent=2))
if __name__=='__main__':main()
