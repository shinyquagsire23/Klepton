#!/usr/bin/env python3
"""Build the pinned protobuf/Brotli sources for the Unity 6.3 mod workspace."""
import argparse
import concurrent.futures
import hashlib
import json
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[3]
def digest(path): return hashlib.sha256(path.read_bytes()).hexdigest()
def build(work, legacy, jobs=2):
    tools = work/'toolchains/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin'
    output = work/'build/network-static'; output.mkdir(parents=True, exist_ok=True)
    # The original 1.0.5 generated protobuf ABI still pins 3.21.12 / Brotli 1.1.0.
    for item in json.loads((ROOT/'ports/scotland2/build-inputs.json').read_text())['sources']:
        if item.get('destination') in {'protobuf','brotli'}:
            if digest(legacy/item['file']) != item['sha256']:
                raise ValueError('Network source archive mismatch: '+item['file'])
    protobuf = legacy/'protobuf'; sources=[]
    for name in ['libprotobuf-lite.cmake','libprotobuf.cmake']:
        text=(protobuf/'cmake'/name).read_text().split(')\n',1)[0]
        sources += [protobuf/p for p in re.findall(r'\$\{protobuf_SOURCE_DIR\}/([^\s]+\.cc)',text)]
    if len(set(sources)) != len(sources) or len(sources)<50:
        raise ValueError('Unexpected protobuf production source list')
    def library(name, sources, flags, c=False):
        folder=output/name; folder.mkdir(exist_ok=True)
        flags += ['-ffile-prefix-map='+str(legacy)+'=/dependencies','-fdebug-compilation-dir=.']
        key=hashlib.sha256(json.dumps(flags).encode())
        root=legacy/('protobuf/src' if name=='protobuf' else 'brotli/c')
        for path in sorted(root.rglob('*')):
            if path.is_file() and path.suffix in {'.h','.inc'}: key.update(path.read_bytes())
        stamp=key.digest()
        def compile_one(source):
            obj=folder/(hashlib.sha256(str(source.relative_to(legacy)).encode()).hexdigest()[:16]+'.o')
            receipt=obj.with_suffix('.key'); expected=hashlib.sha256(stamp+source.read_bytes()).hexdigest()
            if not (obj.exists() and receipt.exists() and receipt.read_text()==expected):
                print('Compiling',name,source.name,flush=True)
                with obj.with_suffix('.log').open('w') as log:
                    result=subprocess.run([str(tools/('aarch64-linux-android26-clang' if c else 'aarch64-linux-android26-clang++')),
                                           *flags,'-c',str(source),'-o',str(obj)],stdout=log,stderr=subprocess.STDOUT)
                if result.returncode: raise RuntimeError(obj.with_suffix('.log').read_text())
                receipt.write_text(expected)
            return obj
        with concurrent.futures.ThreadPoolExecutor(max_workers=max(1,min(4,jobs))) as pool:
            objects=list(pool.map(compile_one,sources))
        archive=output/('lib'+name+'.a')
        subprocess.run([str(tools/'llvm-ar'),'rcsD',str(archive),*[str(p) for p in objects]],check=True)
        return archive
    proto=library('protobuf',sources,['-std=c++17','-O1','-fPIC','-fvisibility=hidden','-DHAVE_PTHREAD=1','-DHAVE_ZLIB=1','-I'+str(protobuf/'src')])
    brotli=legacy/'brotli'
    brot=library('brotlidec',sorted((brotli/'c/common').glob('*.c'))+sorted((brotli/'c/dec').glob('*.c')),
                 ['-std=c99','-O1','-fPIC','-fvisibility=hidden','-I'+str(brotli/'c/include')],c=True)
    receipt={'protobuf':digest(proto),'brotlidec':digest(brot),'status':'Compiled production static dependencies only; no network or game run performed'}
    (output/'build-receipt.json').write_text(json.dumps(receipt,indent=2)+'\n')
    print('Built pinned protobuf and Brotli',flush=True)
    return [proto,brot]
if __name__=='__main__':
    p=argparse.ArgumentParser(description=__doc__);p.add_argument('--workspace',type=Path,required=True)
    p.add_argument('--legacy-dependencies',type=Path,required=True);p.add_argument('--jobs',type=int,default=2)
    a=p.parse_args();build(a.workspace.resolve(),a.legacy_dependencies.resolve(),a.jobs)
