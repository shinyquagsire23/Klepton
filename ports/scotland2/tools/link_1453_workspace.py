#!/usr/bin/env python3
"""Recreate the SDK aliases of a 1.45.3 mod workspace for this checkout.

The developer kit's RESTORE-SDK-LINKS.py with explicit paths: the workspace
(mod-workspace from the kit), the legacy dependency sources and a prepared
NDK r29 (prepare_1453_ndk.py). The Scotland2 sources are this checkout's.
"""
import argparse, os
from pathlib import Path

NATIVE = Path(__file__).resolve().parents[3]

def link(path, target):
    path.parent.mkdir(parents=True, exist_ok=True)
    if path.is_dir() and not path.is_symlink() and not any(path.iterdir()):
        path.rmdir()
    if path.exists() or path.is_symlink():
        if path.resolve() != target.resolve():
            raise ValueError('Conflicting path: ' + str(path))
    else:
        path.symlink_to(os.path.relpath(target, path.parent), target_is_directory=target.is_dir())

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--legacy-dependencies', type=Path, required=True)
    p.add_argument('--ndk', type=Path, required=True)
    a = p.parse_args()
    work, legacy, ndk = a.workspace.resolve(), a.legacy_dependencies.resolve(), a.ndk.resolve()
    if not (ndk / 'SC2-NDK-RECEIPT.json').is_file():
        raise SystemExit('Prepare the NDK with prepare_1453_ndk.py first')
    sdk = work / 'build/sdk'
    sdk.mkdir(parents=True, exist_ok=True)
    for name in ['bs-cordl', 'reflectcpp']:
        link(work / 'sources' / name, work / 'work-sources' / name)
    link(work / 'toolchains/android-ndk-r29', ndk)
    pairs = {'beatsaber-hook': 'bshook', 'custom-types': 'customtypes', 'bsml': 'bsml', 'lapiz': 'lapiz',
             'songcore': 'songcore', 'metacore': 'metacore', 'metacore-bs': 'metacore-bs',
             'playlistcore': 'playlistcore', 'flamingo': 'flamingo', 'web-utils': 'web-utils',
             'song-details': 'song-details', 'beatsaverplusplus': 'beatsaverplusplus',
             'paper2_scotland2': 'paperlog', 'config-utils': 'config-utils',
             'rapidjson-macros': 'rapidjson-macros'}
    for name, target in pairs.items():
        link(sdk / name, work / 'work-sources' / target)
    for prefix in ['shared', 'include']:
        link(sdk / 'reflectcpp' / prefix, work / 'sources/reflectcpp/include')
    link(sdk / 'scotland2', NATIVE / 'ports/scotland2/upstream')
    link(sdk / 'capstone', legacy / 'capstone-qpm/source')
    for name in ['gif-lib', 'tinyxml2', 'rapidjson']:
        link(sdk / name, legacy / name)
    link(sdk / 'libcurl', legacy / 'libcurl-8.5.0')
    link(sdk / 'fmt/fmt/include', work / 'work-sources/fmt/include')
    link(work / 'work-sources/bshook/shared/rapidjson', legacy / 'rapidjson')
    link(work / 'work-sources/paperlog/shared/utfcpp', legacy / 'utfcpp')
    for name in ['EasyGifReader', 'cppcodec', 'css-color-parser-cpp']:
        link(work / 'work-sources/bsml/include/submodules' / name, legacy / name)
    print('SDK aliases point at', work, legacy, ndk)

if __name__ == '__main__':
    main()
