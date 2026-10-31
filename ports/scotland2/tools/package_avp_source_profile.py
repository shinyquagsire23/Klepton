#!/usr/bin/env python3
"""Make a deterministic AVP source/graphics profile, excluding all private inputs."""
import argparse,gzip,hashlib,io,json,subprocess,tarfile
from pathlib import Path
import bootstrap
ROOT=Path(__file__).resolve().parents[3]
def package(output):
    if subprocess.check_output(['git','status','--porcelain','--untracked-files=no'],cwd=ROOT,text=True).strip():
        raise ValueError('Commit the reviewed source before packaging')
    files={}
    for name in subprocess.check_output(['git','ls-files'],cwd=ROOT,text=True).splitlines():
        if name.startswith('ports/scotland2/fakeid/'):continue
        path=ROOT/name
        if path.is_symlink() or not path.is_file():raise ValueError('Source contains a link/non-file: '+name)
        if path.suffix.lower() in {'.apk','.obb','.ipa','.p12','.mobileprovision','.key'} or '.app/' in name:
            raise ValueError('Private/binary game or signing input in source inventory: '+name)
        files[name]=path
    # The native original Scotland2 core uses this independently prepared public
    # fmt input. Keep only its public headers and license, never a dependency cache.
    fmt=ROOT/'port-deps/sources/fmt'
    for path in sorted((fmt/'include').rglob('*')):
        if path.is_file() and not path.is_symlink():files[str(path.relative_to(ROOT))]=path
    license=fmt/'LICENSE.rst'
    if license.is_file():files[str(license.relative_to(ROOT))]=license
    sources={name:bootstrap.digest(path) for name,path in files.items()}
    graphics=json.loads((ROOT/'ports/scotland2/graphics-inputs.json').read_text())['files']
    for row in graphics:
        path=ROOT/row['path']
        if not path.is_file() or bootstrap.digest(path)!=row['sha256']:raise ValueError('Changed public graphics input: '+row['path'])
        files[row['path']]=path
    head=subprocess.check_output(['git','rev-parse','--short=7','HEAD'],cwd=ROOT,text=True).strip()
    ancestors=subprocess.check_output(['git','rev-list','--abbrev-commit','--abbrev=7','HEAD'],cwd=ROOT,text=True).splitlines()
    version=int(subprocess.check_output(['git','rev-list','--count','HEAD'],cwd=ROOT,text=True))
    manifest={'format':1,'version':version,'commit':head,'ancestors':ancestors,'created':'1970-01-01T00:00:00Z',
              'appRevision':version,'modified':True}
    generated={'SOURCE-MANIFEST.json':(json.dumps({'files':sources},sort_keys=True,indent=2)+'\n').encode(),
               'toolchain.json':(json.dumps(manifest,sort_keys=True,indent=2)+'\n').encode()}
    output.parent.mkdir(parents=True,exist_ok=True)
    with output.open('wb') as raw,gzip.GzipFile(filename='',fileobj=raw,mode='wb',mtime=0) as zipped,tarfile.open(fileobj=zipped,mode='w|',format=tarfile.USTAR_FORMAT) as archive:
        for name in sorted(set(files)|set(generated)):
            data=generated[name] if name in generated else files[name].read_bytes()
            item=tarfile.TarInfo(name);item.size=len(data);item.mtime=0;item.uid=item.gid=0
            item.mode=0o755 if name in files and files[name].stat().st_mode&0o111 else 0o644
            archive.addfile(item,io.BytesIO(data))
    manifest.update(archive=output.name,sha256=bootstrap.digest(output),size=output.stat().st_size)
    output.with_name(output.name+'.json').write_text(json.dumps(manifest,sort_keys=True,indent=2)+'\n')
    print('AVP_SOURCE_PROFILE_OK',len(files)+len(generated),'public source/graphics files;',manifest['sha256'],'no game/signing/saves/logs or Fake ID implementation')
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--output',type=Path,required=True)
    package(p.parse_args().output.resolve())
