#!/usr/bin/env python3
"""Extract the pinned NDK 29 arm64 build tools without unrelated debuggers/ABIs."""
import argparse
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import stat
import zipfile

ARCHIVE_SHA256 = 'ce5e4b100ec5fe5be4eb3edcb2c02528824ff9cda3860f5304619be6c3da34d3'
ROOT = 'android-ndk-r29'
PREFIX = 'toolchains/llvm/prebuilt/darwin-x86_64/'
TOOLS = {'clang', 'clang++', 'clang-21', 'ld', 'ld.lld', 'lld', 'llvm-ar',
         'llvm-ranlib', 'llvm-nm', 'llvm-readelf', 'llvm-readobj', 'llvm-objcopy',
         'llvm-strip', 'llvm-objdump', 'llvm-symbolizer', 'llvm-cxxfilt',
         'llvm-addr2line'}

def selected(name):
    if name == 'source.properties' or name.startswith(('meta/', 'build/cmake/')):
        return True
    if not name.startswith(PREFIX):
        return False
    rel = name.removeprefix(PREFIX)
    if rel.startswith('bin/'):
        base = rel.removeprefix('bin/')
        return base in TOOLS or base.startswith('aarch64-linux-android')
    if rel.startswith('sysroot/usr/lib/'):
        return rel.startswith('sysroot/usr/lib/aarch64-linux-android/')
    if rel.startswith(('sysroot/usr/include/', 'lib/clang/21/include/')):
        return True
    if rel.startswith('lib/clang/21/lib/linux/'):
        return 'aarch64' in rel or rel.endswith('/clang_rt.crtbegin-aarch64-android.o')
    if rel.startswith('lib/') and rel.count('/') == 1:
        return rel.endswith('.dylib') and not rel.endswith(('liblldb.dylib', 'libclang.dylib'))
    return False

def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as source:
        for chunk in iter(lambda: source.read(4 << 20), b''):
            h.update(chunk)
    return h.hexdigest()

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    if digest(args.archive) != ARCHIVE_SHA256:
        raise ValueError('NDK archive does not match the verified stable r29 release')
    output = args.output.resolve()
    if output.exists():
        raise ValueError('Refusing to replace an existing toolchain')
    output.mkdir(parents=True)
    files, links = {}, {}
    with zipfile.ZipFile(args.archive) as archive:
        for entry in archive.infolist():
            parts = PurePosixPath(entry.filename).parts
            if not parts or parts[0] != ROOT or '..' in parts:
                raise ValueError('Unsafe NDK archive path')
            relative = str(PurePosixPath(*parts[1:]))
            if entry.is_dir() or not selected(relative):
                continue
            path = output / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            data = archive.read(entry)
            mode = entry.external_attr >> 16
            if stat.S_ISLNK(mode):
                target = data.decode()
                if not (path.parent / target).resolve().is_relative_to(output):
                    raise ValueError('NDK link escapes the output')
                path.symlink_to(target)
                links[relative] = target
            else:
                path.write_bytes(data)
                path.chmod(stat.S_IMODE(mode) or 0o644)
                files[relative] = hashlib.sha256(data).hexdigest()
    if any(not (output / name).exists() for name in links):
        raise ValueError('Selected toolchain has an unresolved link')
    receipt = {'archiveSha256': ARCHIVE_SHA256, 'version': '29.0.14206865',
               'scope': 'Arm64 Android compiler/headers/libraries; unrelated tools and ABIs omitted',
               'files': files, 'links': links}
    (output / 'SC2-NDK-RECEIPT.json').write_text(json.dumps(receipt, indent=2) + '\n')
    print('Verified NDK archive; extracted', len(files), 'files and', len(links), 'links')

if __name__ == '__main__':
    main()
