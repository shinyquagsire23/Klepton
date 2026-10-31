#!/usr/bin/env python3
"""Populate a NEW workspace from hash-pinned public inputs; never use build outputs."""
import argparse,hashlib,json,os,pathlib,shutil,stat,subprocess,tarfile,tempfile,zipfile,io,re
ROOT=pathlib.Path(__file__).resolve().parents[3]
def digest(path):
 h=hashlib.sha256()
 with path.open('rb') as f:
  for block in iter(lambda:f.read(1024*1024),b''):h.update(block)
 return h.hexdigest()

def git_source_archive(repository,commit,names,output):
 """Canonical source-only tar from pinned Git blobs, excluding bundled builds."""
 if not re.fullmatch(r'[0-9a-f]{40}',commit):raise ValueError('Git source needs a full commit ID')
 resolved=subprocess.check_output(['git','-C',str(repository),'rev-parse',commit+'^{commit}'],text=True).strip()
 if resolved!=commit:raise ValueError('Git source revision mismatch')
 if len(names)!=len(set(names)) or len(names)>10000:raise ValueError('Invalid selected Git file list')
 with tarfile.open(output,'w',format=tarfile.USTAR_FORMAT) as tar:
  for name in sorted(names):
   safe_name(name,0)
   entry=subprocess.check_output(['git','-C',str(repository),'ls-tree',commit,'--',name]).decode().strip().split('\t')
   if len(entry)!=2 or entry[1]!=name or entry[0].split()[0] not in {'100644','100755'}:raise ValueError('Not a regular pinned source: '+name)
   data=subprocess.check_output(['git','-C',str(repository),'show',commit+':'+name])
   info=tarfile.TarInfo('source/'+name);info.mode=int(entry[0].split()[0][-3:],8);info.size=len(data);info.mtime=0
   tar.addfile(info,io.BytesIO(data))
def fetch(row,cache,seeds,offline):
 name=row['sha256'];target=cache/name
 if target.exists():
  if digest(target)!=name:raise ValueError('corrupt cached input: '+str(target))
  return target
 cache.mkdir(parents=True,exist_ok=True)
 fd,tmp=tempfile.mkstemp(prefix=name+'.',dir=cache);os.close(fd);tmp=pathlib.Path(tmp)
 try:
  candidate=next((p/row['file'] for p in seeds if (p/row['file']).is_file()),None)
  if candidate:shutil.copyfile(candidate,tmp)
  elif offline:raise ValueError('offline cache lacks '+row['file'])
  elif row.get('gitArchive'):
   source=row['gitArchive'];url=source['repository']
   if not re.fullmatch(r'https://github\.com/[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+(?:\.git)?',url):raise ValueError('Git archive requires a public HTTPS GitHub source')
   with tempfile.TemporaryDirectory(prefix='sc2-git-source-',dir=cache) as directory:
    repo=pathlib.Path(directory)/'repository'
    subprocess.run(['git','init','--bare',str(repo)],check=True,stdout=subprocess.DEVNULL)
    subprocess.run(['git','-C',str(repo),'-c','protocol.file.allow=never','fetch','--depth=1','--filter=blob:none',url,source['commit']],check=True)
    git_source_archive(repo,source['commit'],source['files'],tmp)
  else:subprocess.run(['curl','--fail','--location','--proto','=https','--proto-redir','=https','--tlsv1.2','--retry','2','--connect-timeout','20','--max-time','1800','--output',str(tmp),row['url']],check=True)
  if digest(tmp)!=name:raise ValueError('SHA-256 mismatch: '+row['file'])
  os.replace(tmp,target)
 finally:tmp.unlink(missing_ok=True)
 return target

def safe_name(name,strip):
 if '\\' in name or name.startswith('/') or '\x00' in name:raise ValueError('unsafe archive name')
 parts=pathlib.PurePosixPath(name).parts
 if '..' in parts:raise ValueError('archive traversal')
 parts=parts[strip:]
 return pathlib.Path(*parts) if parts else None

def unpack(archive,dest,strip=1):
 dest=dest.resolve()
 if dest.exists() and any(dest.iterdir()):raise ValueError('archive destination must be empty')
 dest.mkdir(parents=True,exist_ok=True);links=[];total=0
 def put(name,size,mode,reader,link=None):
  nonlocal total
  rel=safe_name(name,strip)
  if rel is None:return
  path=dest/rel
  total+=size
  if total>12*1024**3 or size>4*1024**3:raise ValueError('archive size exceeds bootstrap limit')
  if link is not None:
   target=(path.parent/link).resolve()
   if not target.is_relative_to(dest.resolve()):raise ValueError('archive link escapes root')
   links.append((path,link));return
  if stat.S_ISDIR(mode):path.mkdir(parents=True,exist_ok=True);return
  if not stat.S_ISREG(mode):raise ValueError('unsupported archive special file')
  path.parent.mkdir(parents=True,exist_ok=True)
  with path.open('xb') as out,reader() as inp:shutil.copyfileobj(inp,out,1024*1024)
  if path.stat().st_size!=size:raise ValueError('archive size mismatch')
  path.chmod(0o755 if mode&0o111 else 0o644)
 if zipfile.is_zipfile(archive):
  with zipfile.ZipFile(archive) as z:
   if len(z.infolist())>200000:raise ValueError('excessive archive entry count')
   for e in z.infolist():
    mode=e.external_attr>>16
    if e.is_dir():mode=stat.S_IFDIR|0o755
    elif not stat.S_IFMT(mode):mode|=stat.S_IFREG
    if stat.S_ISLNK(mode):put(e.filename,e.file_size,mode,None,z.read(e).decode())
    else:put(e.filename,e.file_size,mode,lambda e=e:z.open(e))
 else:
  with tarfile.open(archive) as t:
   for i,e in enumerate(t):
    if i>200000:raise ValueError('excessive archive entry count')
    if e.islnk():raise ValueError('archive hard links are unsupported')
    mode=(stat.S_IFDIR if e.isdir() else stat.S_IFREG if e.isfile() else stat.S_IFLNK if e.issym() else 0)|e.mode
    put(e.name,e.size,mode,lambda e=e:t.extractfile(e),e.linkname if e.issym() else None)
 # Defer links until every ordinary file is written, so later entries cannot
 # write through one of the archive's own symbolic links.
 for path,link in links:
  path.parent.mkdir(parents=True,exist_ok=True)
  if path.parent.resolve()!=path.parent.absolute() or not (path.parent/link).resolve().is_relative_to(dest.resolve()):raise ValueError('archive link chain escapes or aliases a parent')
  path.symlink_to(link)
 for path,link in links:
  if not path.resolve().is_relative_to(dest.resolve()):raise ValueError('archive link chain escapes root')

def bootstrap(work,cache,seeds,offline,ndk=None):
 lock=json.loads((ROOT/'ports/scotland2/build-inputs.json').read_text())
 dep=work/'port-deps/sources';dep.mkdir(parents=True,exist_ok=True)
 for row in lock['sources']:
  print('Verify/extract',row['file'],flush=True)
  path=fetch(row,cache,seeds,offline)
  if row['destination']:
   dest=dep/row['destination']
   if dest.exists():raise ValueError('bootstrap requires empty dependency destinations: '+str(dest))
   unpack(path,dest,row['stripComponents'])
   if row['patch']:subprocess.run(['patch','--batch','--forward','-d',str(dest),'-p1','-i',str(ROOT/row['patch'])],check=True)
  target=dep/row['file'];target.parent.mkdir(parents=True,exist_ok=True)
  if not target.exists():target.symlink_to(path)
 # paperlog's public git submodule is a separately pinned utfcpp archive.
 link=dep/'paperlog/shared/utfcpp'
 if link.is_dir() and not any(link.iterdir()):link.rmdir()
 if not link.exists():link.symlink_to(dep/'utfcpp',target_is_directory=True)
 selection=json.loads((ROOT/'ports/scotland2/selected-mods.json').read_text())
 original=json.loads((ROOT/'port-results/coremods/coremods-lock.json').read_text())
 for row in selection['packages']:
  if row.get('sourceBuilt'):continue
  request={**row,'file':row['archive']}
  data=fetch(request,cache,seeds,offline)
  with zipfile.ZipFile(data) as z:
   for binary in original['binaries']:
    if binary['mod']!=row['id']:continue
    dest=work/'port-results/coremods'/binary['file'];dest.parent.mkdir(parents=True,exist_ok=True)
    name=pathlib.Path(binary['file']).name
    matches=[n for n in z.namelist() if pathlib.PurePosixPath(n).name==name]
    if len(matches)!=1:raise ValueError('ambiguous QMOD member: '+name)
    content=z.read(matches[0])
    if hashlib.sha256(content).hexdigest()!=binary['sha256']:raise ValueError('QMOD binary mismatch: '+name)
    dest.write_bytes(content)
 if ndk:
  # Sharing an independently bootstrapped toolchain is an explicit input. Its
  # receipt binds its complete extracted file tree to the pinned NDK archive.
  verify_toolchain(ndk,lock['ndk']['sha256']);target=work/'port-deps/toolchains/android-ndk-r27c'
  target.parent.mkdir(parents=True,exist_ok=True);target.symlink_to(ndk,target_is_directory=True)
 else:
  row={**lock['ndk'],'file':'android-ndk-r27c-darwin.zip'};path=fetch(row,cache,seeds,offline)
  target=work/'port-deps/toolchains/android-ndk-r27c';unpack(path,target,1)
  files=tree_hashes(target)
  (target/'.sc2-toolchain.json').write_text(json.dumps({'archiveSha256':row['sha256'],'files':files},sort_keys=True)+'\n')
 (work/'build-input-receipt.json').write_text(json.dumps({'manifestSha256':digest(ROOT/'ports/scotland2/build-inputs.json'),'dependencyTree':tree_hashes(dep),'inputs':[r['sha256'] for r in lock['sources']],'ndk':lock['ndk']['sha256'],'selectionSha256':digest(ROOT/'ports/scotland2/selected-mods.json')},indent=2)+'\n')

def tree_hashes(root):
 return {str(p.relative_to(root)):{'link':os.readlink(p)} if p.is_symlink() else {'sha256':digest(p),'executable':bool(p.stat().st_mode&0o111)} for p in sorted(root.rglob('*')) if (p.is_file() or p.is_symlink()) and p.name!='.sc2-toolchain.json'}
def verify_toolchain(root,archive):
 receipt=json.loads((root/'.sc2-toolchain.json').read_text())
 if receipt['archiveSha256']!=archive or receipt['files']!=tree_hashes(root):raise ValueError('toolchain tree differs from verified extraction')

def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=pathlib.Path,required=True);p.add_argument('--cache',type=pathlib.Path,required=True);p.add_argument('--seed',type=pathlib.Path,action='append',default=[]);p.add_argument('--offline',action='store_true');p.add_argument('--verified-ndk',type=pathlib.Path)
 a=p.parse_args();bootstrap(a.workspace.resolve(),a.cache.resolve(),[x.resolve() for x in a.seed],a.offline,a.verified_ndk)


def materialize_sources(work,cache,seeds,offline):
 """Restore source-only packages from independently pinned original archives."""
 lock=json.loads((ROOT/'ports/scotland2/source-inputs.json').read_text());receipt={}
 for row in lock['sources']:
  print('Verify original source',row['target'],flush=True)
  archive=fetch(row,cache,seeds,offline)
  if row['patch'] and digest(ROOT/row['patch'])!=row['patchSha256']:raise ValueError('source patch differs from manifest: '+row['patch'])
  with tempfile.TemporaryDirectory(prefix='sc2-source-') as tmp:
   tree=pathlib.Path(tmp)/'tree';unpack(archive,tree,1)
   if row['patch']:subprocess.run(['patch','--batch','--forward','-d',str(tree),'-p1','-i',str(ROOT/row['patch'])],check=True,stdout=subprocess.DEVNULL)
   for name,expected in row['files'].items():
    rel=safe_name(name,0)
    if rel is None:raise ValueError('empty source path')
    source=tree/rel
    data=source.read_bytes()
    if hashlib.sha256(data).hexdigest()!=expected['sha256']:
     normalized=data.replace(b'\r\n',b'\n')
     if expected['lineEndings']=='binary' or hashlib.sha256(normalized).hexdigest()!=expected['normalizedSha256']:raise ValueError('adapted source differs from manifest: '+name)
     data=normalized.replace(b'\n',b'\r\n') if expected['lineEndings']=='crlf' else normalized
     if hashlib.sha256(data).hexdigest()!=expected['sha256']:raise ValueError('source line ending mismatch: '+name)
    dest=work/row['target']/rel
    if not dest.resolve().is_relative_to(work.resolve()):raise ValueError('source destination escapes workspace')
    if dest.exists():
     if digest(dest)!=expected['sha256']:raise ValueError('existing vendored source changed: '+str(dest))
    else:dest.parent.mkdir(parents=True,exist_ok=True);dest.write_bytes(data);dest.chmod(source.stat().st_mode&0o777)
    receipt[str(dest.relative_to(work))]=expected['sha256']
 (work/'source-materialization.json').write_text(json.dumps({'files':receipt},sort_keys=True)+'\n')

if __name__=='__main__':main()
