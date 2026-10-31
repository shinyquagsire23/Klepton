#!/usr/bin/env python3
"""Compare every vendored file against its verified original source archive.

This verifies identity and adaptation coverage; manual hunk review is separate.
"""
import argparse,json,pathlib,subprocess,tempfile
import bootstrap
ROOT=pathlib.Path(__file__).resolve().parents[3]
def main():
 p=argparse.ArgumentParser(description=__doc__);p.add_argument('--cache',type=pathlib.Path,required=True);p.add_argument('--seed',type=pathlib.Path,action='append',default=[]);p.add_argument('--refresh-patches',action='store_true');p.add_argument('--report',type=pathlib.Path,required=True);a=p.parse_args()
 lock=json.loads((ROOT/'ports/scotland2/source-inputs.json').read_text());report=[]
 tracked=subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines()
 for row in lock['sources']:
  archive=bootstrap.fetch(row,a.cache.resolve(),[s.resolve() for s in a.seed],True)
  with tempfile.TemporaryDirectory(prefix='sc2-source-audit-') as tmp:
   original=pathlib.Path(tmp)/'original';bootstrap.unpack(archive,original,1)
   files=[];patch=[]
   for name in sorted(set(tracked) | {str(pathlib.PurePosixPath(row['target'])/n) for n in row['files']}):
    target=ROOT/name
    try:relative=target.relative_to(ROOT/row['target'])
    except ValueError:continue
    if row['includePrefixes'] and not any(str(relative).startswith(prefix) for prefix in row['includePrefixes']):continue
    if not target.is_file():continue
    source=original/relative
    if not source.is_file():raise ValueError('unaccounted vendored file: '+name)
    old=source.read_bytes();new=target.read_bytes();equivalent=old.replace(b'\r\n',b'\n')==new.replace(b'\r\n',b'\n')
    files.append({'path':name,'originalSha256':bootstrap.digest(source),'currentSha256':bootstrap.digest(target),'classification':'original (line endings may differ)' if equivalent else 'adapted; manual hunk review required'})
    if not equivalent:
     diff=subprocess.run(['diff','-u','--label','a/'+str(relative),'--label','b/'+str(relative),str(source),str(target)],capture_output=True)
     if diff.returncode!=1:raise ValueError('failed source diff: '+name)
     patch.append(diff.stdout)
   if patch and not row['patch']:raise ValueError('missing patch mapping: '+row['target'])
   if a.refresh_patches and row['patch']:(ROOT/row['patch']).write_bytes(b''.join(patch))
   # Reconstruct adapted files using only declared original inputs + patch.
   if row['patch']:
    subprocess.run(['patch','--batch','--forward','-d',str(original),'-p1','-i',str(ROOT/row['patch'])],check=True,capture_output=True)
   for file in files:
    relative=pathlib.Path(file['path']).relative_to(row['target'])
    if (original/relative).read_bytes().replace(b'\r\n',b'\n')!=(ROOT/file['path']).read_bytes().replace(b'\r\n',b'\n'):raise ValueError('patch does not reconstruct '+file['path'])
   report.append({'target':row['target'],'archiveSha256':row['sha256'],'commit':row['commit'],'patchSha256':bootstrap.digest(ROOT/row['patch']) if row['patch'] else None,'files':files})
 a.report.write_text(json.dumps({'scope':'Original source identity and adaptation reconstruction; not a substitute for code review','trees':report},indent=2)+'\n')
 print('PASS',len(report),'source trees,',sum(len(x['files']) for x in report),'files independently identified and reconstructed')
if __name__=='__main__':main()
