#!/usr/bin/env python3
"""Stage the Beat Saber mods for an AVP Play build (run.sh with KLEPTON_SCOTLAND2=1).

The toolchain carries the mods prebuilt, per game version (ports/scotland2/
avp-profiles.json): the compiled guest libraries, their hook catalog and the
native Scotland2 loader. Nothing is compiled here and nothing is downloaded.
This picks the profile by the game's version code, checks the owner's game
files against it, translates the libraries with this checkout's klepton-ld,
and prepares the hook gates in the game's own libil2cpp/libunity before Xcode
signs the app. A version without a profile stops with the reason.

Runs with the system Python (AVP Play builds with PATH=/usr/bin:/bin).
Prints SC2_GAME_PROFILE=<name> as its last line for run.sh.
"""
import argparse, json, plistlib, re, shutil, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(Path(__file__).resolve().parent))
import bootstrap
from prepare_1453_game_hooks import game_profiles

PORT = ROOT / 'ports/scotland2'
FRAMEWORKS = ROOT / 'visionos/Frameworks'
METADATA = ROOT / 'visionos/Scotland2Metadata'


def fail(text):
    print('!! mods: ' + text, file=sys.stderr)
    sys.exit(1)


def run(args):
    subprocess.run([str(a) for a in args], cwd=ROOT, check=True, stdout=subprocess.DEVNULL)


def version_code(tree):
    text = (tree / 'apktool.yml').read_text()
    match = re.search(r'^\s*versionCode:\s*[\'"]?(\d+)[\'"]?\s*$', text, re.M)
    if not match:
        fail('the unpacked game names no versionCode (apktool.yml)')
    return int(match.group(1))


def profile_for(target, code):
    profiles = json.loads((PORT / 'avp-profiles.json').read_text())['profiles']
    for row in profiles:
        if row['target'] == target and row['versionCode'] == code:
            return row
    made = ', '.join(r['versionName'] for r in profiles if r['target'] == target)
    fail(f'not ported to version code {code} of {target} yet (they exist for {made or "no version"})')


def framework(folder, name, identifier):
    bundle = folder / (name + '.framework')
    bundle.mkdir(parents=True)
    (bundle / 'Info.plist').write_bytes(plistlib.dumps({
        'CFBundleExecutable': name, 'CFBundleIdentifier': identifier, 'CFBundleName': name,
        'CFBundlePackageType': 'FMWK', 'CFBundleShortVersionString': '1.0', 'CFBundleVersion': '1',
        'MinimumOSVersion': '26.0'}))
    return bundle / name


def xcframework(source, target):
    if target.exists():
        shutil.rmtree(target)
    run(['xcodebuild', '-create-xcframework', '-framework', source, '-output', target])


def translate(ld, elf, binary):
    run([ld, elf, '--platform', 'visionos', '--install-name',
         '@rpath/' + binary.parent.name + '/' + binary.name, '-o', binary, '--quiet'])


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--target', required=True)
    p.add_argument('--tree', type=Path, required=True, help='the unpacked game (e.g. beatsaber140)')
    a = p.parse_args()
    tree = a.tree.resolve()
    ld = ROOT / 'build/klepton-ld'
    if not ld.is_file():
        fail('build/klepton-ld is missing (make build/klepton-ld)')

    row = profile_for(a.target, version_code(tree))
    selection = json.loads((PORT / row['selection']).read_text())
    prebuilt = (PORT / row['prebuilt']).resolve()
    if not prebuilt.is_relative_to(PORT.resolve()):
        fail('the profile points outside the port')
    if selection.get('excludedMods') != ['FakeId'] or any('fakeid' in m.lower() for m in selection['metadata']):
        fail('Fake ID must stay excluded')

    # The owner's game must be exactly the version the hooks were measured on.
    for name, expected in selection['game']['files'].items():
        relative = ('assets/bin/Data/Managed/Metadata/' if name == 'global-metadata.dat' else 'lib/arm64-v8a/') + name
        if bootstrap.digest(tree / relative) != expected:
            fail(f'{name} is not the one of {row["versionName"]}; the mods need exactly that version')
    # ...and the prebuilt pack exactly what was packed.
    pack = json.loads((prebuilt / 'pack.json').read_text())
    for relative, expected in pack['files'].items():
        if bootstrap.digest(prebuilt / relative) != expected:
            fail('the prebuilt mods changed after packing: ' + relative)
    combined = json.loads((prebuilt / 'hooks/combined-targets.json').read_text())
    if combined['game'] != selection['game']:
        fail('the hook catalog belongs to another game version')
    for module, expected in combined['moduleHashes'].items():
        library = pack['catalogs'][module]
        if bootstrap.digest(prebuilt / 'libs' / library) != expected:
            fail('the hook catalog does not match the library ' + library)

    # Generated output of earlier builds goes: it is rebuilt from the pack every time.
    for old in list(FRAMEWORKS.glob('SC2_*.xcframework')) + [FRAMEWORKS / 'Scotland2Core.xcframework']:
        if old.exists():
            shutil.rmtree(old)
    if METADATA.exists():
        shutil.rmtree(METADATA)
    FRAMEWORKS.mkdir(exist_ok=True)

    with tempfile.TemporaryDirectory(prefix='sc2-avp-') as scratch:
        work = Path(scratch)
        rows = []
        for relative in selection['metadata']:
            name = Path(relative).name
            source = prebuilt / 'libs' / name
            destination = METADATA / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            stem = Path(name).stem
            binary = framework(work / 'mods', stem, 'dev.klepton.scotland2.' + stem.replace('_', '-'))
            translate(ld, source, binary)
            xcframework(binary.parent, FRAMEWORKS / ('SC2_' + stem + '.xcframework'))
            rows.append({'metadata': relative, 'elfSha256': bootstrap.digest(source),
                         'translationSha256': bootstrap.digest(binary)})
        binary = framework(work / 'mods', 'libcxx_shared', 'dev.klepton.scotland2.libcxx-shared')
        translate(ld, prebuilt / 'libs/libc++_shared.so', binary)
        xcframework(binary.parent, FRAMEWORKS / 'SC2_libcxx_shared.xcframework')
        xcframework(prebuilt / 'Scotland2Core.framework', FRAMEWORKS / 'Scotland2Core.xcframework')
        shutil.copytree(PORT / 'certificates', METADATA / 'certificates')
        # The mods' authors, shown in the start window (Settings › Mods).
        shutil.copyfile(PORT / 'credits.json', METADATA / 'credits.json')
        (METADATA / 'provenance.json').write_text(json.dumps({
            'schema': 1, 'profile': selection['runtimeProfile'],
            'selectionSha256': bootstrap.digest(PORT / row['selection']),
            'mode': 'release', 'excludedMods': ['FakeId'], 'libraries': rows}, indent=2) + '\n')

        # The game's own libraries, with the measured hook gates prepared before signing.
        targets = 0
        for name, profile in game_profiles(combined).items():
            raw = tree / 'lib/arm64-v8a' / (name + '.so')
            plain = work / 'game' / (name + '.unprepared.dylib')
            plain.parent.mkdir(parents=True, exist_ok=True)
            run([ld, raw, '--platform', 'visionos', '--install-name', f'@rpath/{name}.framework/{name}',
                 '-o', plain, '--quiet'])
            manifest = work / 'game' / (name + '-manifest.json')
            manifest.write_text(json.dumps({'schema': 1, 'elf_sha256': profile['sha256'],
                                            'translation_sha256': bootstrap.digest(plain),
                                            'targets': profile['targets'],
                                            'originalEntryDepth': profile['originalEntryDepth']}, indent=2) + '\n')
            binary = framework(work / 'game', name, 'dev.klepton.scotland2.game.' + name)
            run([sys.executable, PORT / 'tools/prepare_hooks.py', '--elf', raw, '--input', plain,
                 '--manifest', manifest, '--output', binary])
            (binary.parent / (name + '.hooks.json')).unlink(missing_ok=True)
            xcframework(binary.parent, FRAMEWORKS / a.target / (name + '.xcframework'))
            targets += len(profile['targets'])

    print(f'  mods: {row["versionName"]}, {len(rows)} modules, {targets} game entries prepared')
    print('SC2_GAME_PROFILE=' + row['name'])


if __name__ == '__main__':
    try:
        main()
    except (OSError, KeyError, ValueError, subprocess.CalledProcessError) as error:
        fail(str(error))
