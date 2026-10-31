"""Exercise the exact extraction adapter and pinned decompressor on host fixtures."""
import ctypes,io,pathlib,sys,tempfile,zipfile,subprocess,os
executable=not sys.argv[1].endswith(".dylib")
lib=ctypes.CDLL(sys.argv[1]) if not executable else None
extract=lib.zip_stream_extract if lib else None
if extract:extract.argtypes=[ctypes.c_void_p,ctypes.c_size_t,ctypes.c_char_p,ctypes.c_void_p,ctypes.c_void_p];extract.restype=ctypes.c_int
def archive(entries):
 out=io.BytesIO()
 with zipfile.ZipFile(out,'w',zipfile.ZIP_DEFLATED) as z:
  for name,data,mode in entries:
   i=zipfile.ZipInfo(name);i.external_attr=mode<<16;i.compress_type=zipfile.ZIP_DEFLATED;z.writestr(i,data)
 return out.getvalue()
def run(data,path):
 if extract:return extract(data,len(data),bytes(path),None,None)
 with tempfile.NamedTemporaryFile() as archive:
  archive.write(data);archive.flush()
  result=subprocess.run([sys.argv[1],archive.name,str(path)],capture_output=True,env=dict(os.environ,ASAN_OPTIONS='abort_on_error=1',UBSAN_OPTIONS='halt_on_error=1'))
  assert result.returncode in (0,1) and b'AddressSanitizer' not in result.stderr and b'runtime error:' not in result.stderr,result.stderr
  return result.returncode
with tempfile.TemporaryDirectory() as folder:
 root=pathlib.Path(folder)
 good=archive([('Info.dat',b'{}',0o100644),('sub/song.egg',b'audio',0o100644)])
 assert run(good,root/'map')==0
 assert (root/'map/sub/song.egg').read_bytes()==b'audio'
 before={str(p):p.read_bytes() for p in (root/'map').rglob('*') if p.is_file()}
 assert run(good,root/'map')!=0
 assert before=={str(p):p.read_bytes() for p in (root/'map').rglob('*') if p.is_file()}
 cases=[archive([(name,b'bad',mode)]) for name,mode in [('..'+'/escape',0o100644),('/absolute',0o100644),('x/../../escape',0o100644),('a\\b',0o100644),('C:bad',0o100644),('link',0o120777),('pipe',0o010644),('./file',0o100644)]]
 cases += [archive([('/'.join(['nested']*40)+'/file',b'bad',0o100644)]),good[:-10],b'not zip',archive([('a',b'x',0o100644),('A',b'y',0o100644)]),archive([('bomb',b'0'*2000000,0o100644)])]
 for index,data in enumerate(cases):
  dest=root/str(index);assert run(data,dest)!=0,index;assert not dest.exists(),index
 assert not (root/'escape').exists()
 assert not list(root.glob('.sc2-download-*'))
 # Destination link and nonempty folder are preserved byte-for-byte.
 (root/'link').symlink_to(root/'map',target_is_directory=True)
 assert run(good,root/'link')!=0 and (root/'link').is_symlink()
 print('PASS real ZIP extraction: nested map, existing songs preserved, traversal/absolute/links/special files/collisions/bomb/corruption rejected, stages cleaned')
