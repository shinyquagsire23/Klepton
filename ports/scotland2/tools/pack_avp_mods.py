#!/usr/bin/env python3
"""Pack the built mods of one game version for the toolchain (ports/scotland2/prebuilt/<version code>).

Developer side only: it takes a mod workspace built with the official NDK r29
(build_1453_support/_flamingo/_stack), checks every library against the
recorded reference outputs, measures the hook catalog against the owner's
game files, compiles the native Scotland2 loader and writes pack.json with the
hash of every packed file. stage_avp_mods.py uses the result on the user's Mac,
where nothing has to be compiled.
"""
import argparse, json, re, shutil, subprocess, sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootstrap
from collect_1453_hooks import collect, CATALOGS

PORT = ROOT / 'ports/scotland2'


def same_code(work, ours, theirs):
    """The same machine code apart from where strings landed: a library that embeds a source path (__FILE__ of
    a dependency built outside the prefix maps) differs from the reference only in that path and in the
    offsets that point past it. Same instruction count, opcodes and registers; immediates are masked."""
    if not theirs.is_file():
        return False
    tools = work / 'toolchains/android-ndk-r29/toolchains/llvm/prebuilt/darwin-x86_64/bin'
    def listing(path):
        text = subprocess.check_output([str(tools / 'llvm-objdump'), '-d', '--no-show-raw-insn', '--no-leading-addr',
                                        str(path)], text=True).split('\n')[3:]
        rows = []
        for line in text:
            line = re.sub(r'<[^>]*>', '', line)
            line = re.sub(r'#[^,\]\s]*', '#', line)
            rows.append(re.sub(r'\b0x[0-9a-f]+\b|\b[0-9a-f]{5,}\b', '', line).strip())
        return rows
    def section(path, name):
        return subprocess.check_output([str(tools / 'llvm-readelf'), '-x', name, str(path)], text=True).split('\n', 2)[-1]
    return listing(ours) == listing(theirs) and section(ours, '.dynstr') == section(theirs, '.dynstr')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--workspace', type=Path, required=True)
    p.add_argument('--reference', type=Path, required=True, help='reproducibility-recorded.json of the kit')
    p.add_argument('--reference-libs', type=Path,
                   help='the reference libraries themselves, to tell a path-only difference from a real one')
    p.add_argument('--changed-here', action='append', default=[],
                   help='a library rebuilt with a follow-up patch of this port (port1453/patches/<module>-*.patch), '
                        'which therefore differs from the reference on purpose')
    p.add_argument('--profile', default='1.45.3')
    a = p.parse_args()
    work = a.workspace.resolve()
    profile = next(r for r in json.loads((PORT / 'avp-profiles.json').read_text())['profiles'] if r['name'] == a.profile)
    selection = json.loads((PORT / profile['selection']).read_text())
    game = ROOT / profile['target']
    out = PORT / profile['prebuilt']
    if out.exists():
        shutil.rmtree(out)
    (out / 'libs').mkdir(parents=True)

    record = json.loads(a.reference.read_text())
    if record['manifestSha256'] != bootstrap.digest(PORT / 'port1453/inputs.json'):
        raise SystemExit('the reference belongs to another source manifest')
    reference = {row['library']: row['sha256'] for row in record['libraries']}
    names = sorted({Path(m).name for m in selection['metadata']} | {'libc++_shared.so'})
    code_only = []
    for name in names:
        source = work / 'build/libs' / name
        if name in a.changed_here:
            pass
        elif name in reference and bootstrap.digest(source) != reference[name]:
            if not a.reference_libs or not same_code(work, source, a.reference_libs / name):
                raise SystemExit('differs from the recorded reference: ' + name)
            code_only.append(name)
        shutil.copyfile(source, out / 'libs' / name)

    collect(work, game)
    hooks = out / 'hooks'
    hooks.mkdir()
    for path in sorted((work / 'build/hooks-1453').glob('*.json')):
        shutil.copyfile(path, hooks / path.name)

    subprocess.run(['bash', 'ports/scotland2/build.sh', 'xros'], cwd=ROOT, check=True,
                   env={**__import__('os').environ, 'SC2_BUILD_MODE': 'release'})
    core = out / 'Scotland2Core.framework'
    core.mkdir()
    import plistlib
    (core / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleExecutable': 'Scotland2Core', 'CFBundleIdentifier': 'dev.klepton.scotland2.Scotland2Core',
        'CFBundleName': 'Scotland2Core', 'CFBundlePackageType': 'FMWK', 'CFBundleShortVersionString': '1.0',
        'CFBundleVersion': '1', 'MinimumOSVersion': '26.0'}))
    sdk = subprocess.check_output(['xcrun', '--sdk', 'xros', '--show-sdk-path'], text=True).strip()
    subprocess.run(['xcrun', 'clang++', '-target', 'arm64-apple-xros26.0', '-isysroot', sdk, '-dynamiclib',
                    '-Wl,-force_load,' + str(ROOT / 'build/scotland2/xros/libscotland2-core.a'),
                    '-install_name', '@rpath/Scotland2Core.framework/Scotland2Core',
                    '-o', str(core / 'Scotland2Core')], cwd=ROOT, check=True)
    # The linker's debug map names every object by its absolute path on this Mac; the package must not.
    subprocess.run(['xcrun', 'strip', '-S', str(core / 'Scotland2Core')], check=True)

    leaked = [str(p.relative_to(out)) for p in out.rglob('*') if p.is_file() and str(Path.home()).encode() in p.read_bytes()]
    if leaked:
        raise SystemExit('the package names this Mac\'s home folder: ' + ', '.join(leaked))
    files = {str(path.relative_to(out)): bootstrap.digest(path) for path in sorted(out.rglob('*')) if path.is_file()}
    matched = sorted(n for n in names if n in reference)
    (out / 'pack.json').write_text(json.dumps({
        'schema': 1, 'profile': profile['name'], 'game': selection['game'],
        'referenceMatched': [n for n in matched if n not in code_only and n not in a.changed_here],
        'referenceSameCode': code_only,
        'changedHere': {n: sorted(p.name for p in (PORT / 'port1453/patches').glob(Path(n).stem.removeprefix('lib') + '-*.patch'))
                        for n in a.changed_here},
        'catalogs': CATALOGS, 'files': files}, indent=2) + '\n')
    identical = [n for n in matched if n not in code_only and n not in a.changed_here]
    print(f'packed {len(names)} libraries ({len(identical)} byte-identical to the reference, '
          f'{len(a.changed_here)} changed here on purpose: {", ".join(a.changed_here) or "none"}, '
          f'{len(code_only)} the same code with other embedded paths: {", ".join(code_only) or "none"}), '
          f'hook catalog and loader into {out}')


if __name__ == '__main__':
    main()
