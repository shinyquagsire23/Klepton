#!/usr/bin/env python3
"""Exercise hostile public-archive layouts without using game/signing inputs."""
import io,pathlib,stat,sys,tarfile,tempfile,zipfile
sys.path.insert(0,str(pathlib.Path(__file__).resolve().parents[1]/'tools'))
import bootstrap
with tempfile.TemporaryDirectory(prefix='sc2-bootstrap-test-') as tmp:
 root=pathlib.Path(tmp)
 def archive(rows):
  content=io.BytesIO()
  with zipfile.ZipFile(content,'w') as z:
   for name,data,kind in rows:
    info=zipfile.ZipInfo('root/'+name);info.create_system=3;info.external_attr=(kind|0o755)<<16;z.writestr(info,data)
  path=root/'fixture.zip';path.write_bytes(content.getvalue());return path
 def reject(rows):
  target=root/'bad'
  try:bootstrap.unpack(archive(rows),target,1)
  except (ValueError,FileExistsError):pass
  else:raise AssertionError('accepted unsafe archive layout')
  assert not (root/'escape').exists()
  import shutil
  if target.exists():shutil.rmtree(target)
 good=root/'good';bootstrap.unpack(archive([('file',b'original',stat.S_IFREG),('alias',b'file',stat.S_IFLNK)]),good,1)
 assert (good/'alias').read_bytes()==b'original'
 try:bootstrap.unpack(root/'fixture.zip',good,1)
 except ValueError:pass
 else:raise AssertionError('overwrote a populated destination')
 assert (good/'file').read_bytes()==b'original'
 reject([('../escape',b'bad',stat.S_IFREG)])
 reject([('link',b'../../escape',stat.S_IFLNK)])
 reject([('a',b'b',stat.S_IFLNK),('b',b'../../escape',stat.S_IFLNK)])
 reject([('parent',b'.',stat.S_IFLNK),('parent/file',b'bad',stat.S_IFREG)])
 reject([('duplicate',b'a',stat.S_IFREG),('duplicate',b'b',stat.S_IFREG)])
 tar=root/'hardlink.tar'
 with tarfile.open(tar,'w') as t:
  info=tarfile.TarInfo('root/hard');info.type=tarfile.LNKTYPE;info.linkname='../../escape';t.addfile(info)
 try:bootstrap.unpack(tar,root/'tar',1)
 except ValueError:pass
 else:raise AssertionError('accepted a hard link')
 print('PASS bootstrap extraction: ordinary files/internal links, traversal/escaping chains/parent aliases/duplicates/hardlinks/nonempty destinations')
