#!/usr/bin/env python3
"""Translate a Linux target's libraries for the device and wrap them as frameworks.

mkguest.sh's counterpart for a target whose guest is a Linux process (kind
"linux" in targets.py). The differences from an Android guest are all on the
input side:

  * The libraries do not come from one directory. They are the game's, the
    Steam Linux Runtime's and xrizer's, and which of several hundred are needed
    is not a property of any file — it is what the process loads. So the input
    is a LIST, visionos/lx/<target>.loaded, recorded by a host run:

        KL_LX_LOADLOG=$PWD/visionos/lx/alyx.loaded tools/lx_alyx.sh -vr -vulkan ...

    One line per image: translation name, tab, guest path.

  * A translation is named by file name AND content (runtime/linux/kl_lx_name.h),
    which is the name the runtime will ask the bundle for. klepton-ld
    --print-name is the authority; the name in the list is only checked
    against it, because a library that changed since the list was recorded has
    a different name now and the bundle must carry the one that will be asked
    for.

Output is the same shape mkguest.sh produces — one xcframework per library in
Frameworks/<target>/ — plus Frameworks/<target>/LIBS, the list targets.py reads
back as the target's `libs`. Device slice only: a Linux guest has not been run
in the simulator and the xcframeworks do not pretend otherwise.

Incremental: a framework newer than its ELF and than klepton-ld is left alone.
"""
import concurrent.futures
import os
import pathlib
import plistlib
import shutil
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import targets as targets_mod  # noqa: E402

KLT = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or targets_mod.DEFAULT)
LD = ROOT / "build" / "klepton-ld"
OUT = HERE / "Frameworks" / KLT["name"]
LIST = HERE / "lx" / f"{KLT['name']}.loaded"

# Guest path prefix -> where that tree is on this machine. The same mounts
# tools/lx_alyx.sh makes, longest prefix first.
SYSROOT = pathlib.Path(os.environ.get("KL_LX_SYSROOT", ROOT.parent / "steamrt/root/files"))
GAME = pathlib.Path(os.environ.get("KL_LX_GAME", ROOT.parent / "alyx/game"))
XRIZER = pathlib.Path(os.environ.get("KL_LX_XRIZER", ROOT.parent / "guestbuild/xrizer"))
MOUNTS = [("/opt/xrizer/", XRIZER), ("/game/", GAME), ("/usr/", SYSROOT),
          ("/lib/", SYSROOT / "lib"), ("/bin/", SYSROOT / "bin"), ("/etc/", SYSROOT / "etc")]


def host_path(guest):
    for prefix, host in MOUNTS:
        if guest.startswith(prefix):
            return host / guest[len(prefix):]
    return None


def fw_name(translation):
    # '+' is not a character a framework name survives a build with; the
    # runtime makes the same substitution when it looks (kl_lx_find_dylib).
    return translation.removesuffix(".dylib").replace("+", "x")


def build(item):
    logged, guest = item
    elf = host_path(guest)
    if elf is None or not elf.is_file():
        return None, f"!! {guest}: not found on this machine ({elf})"
    name = subprocess.run([str(LD), "--print-name", str(elf)], capture_output=True,
                          text=True).stdout.strip()
    if not name:
        return None, f"!! {elf}: klepton-ld --print-name said nothing"
    note = ""
    if name != logged:
        note = f"   {guest}: recorded as {logged}, is {name} now — using the current file"
    fw = fw_name(name)
    xc = OUT / f"{fw}.xcframework"
    fdir = xc / "xros-arm64" / f"{fw}.framework"
    binary = fdir / fw
    if (binary.exists() and binary.stat().st_mtime > elf.stat().st_mtime
            and binary.stat().st_mtime > LD.stat().st_mtime):
        return fw, note
    shutil.rmtree(xc, ignore_errors=True)
    fdir.mkdir(parents=True)
    r = subprocess.run([str(LD), "--abi", "linux", "--quiet", "--platform", "visionos",
                        "--install-name", f"@rpath/{fw}.framework/{fw}",
                        str(elf), "-o", str(binary)], capture_output=True, text=True)
    if r.returncode != 0:
        shutil.rmtree(xc, ignore_errors=True)
        last = (r.stderr.strip().splitlines() or ["(no message)"])[-1]
        return None, f"!! {elf}: klepton-ld failed: {last}"
    ident = "".join(c if c.isalnum() or c in ".-" else "-" for c in fw)
    with open(fdir / "Info.plist", "wb") as f:
        plistlib.dump({
            "CFBundleExecutable": fw,
            "CFBundleIdentifier": f"dev.klepton.guest.{ident}",
            "CFBundleName": fw,
            "CFBundlePackageType": "FMWK",
            "CFBundleShortVersionString": "1.0",
            "CFBundleVersion": "1",
            "MinimumOSVersion": "26.0",
        }, f)
    with open(xc / "Info.plist", "wb") as f:
        plistlib.dump({
            "AvailableLibraries": [{
                "BinaryPath": f"{fw}.framework/{fw}",
                "LibraryIdentifier": "xros-arm64",
                "LibraryPath": f"{fw}.framework",
                "SupportedArchitectures": ["arm64"],
                "SupportedPlatform": "xros",
            }],
            "CFBundlePackageType": "XFWK",
            "XCFrameworkFormatVersion": "1.0",
        }, f)
    return fw, note


def main():
    if KLT["kind"] != "linux":
        print(f"!! {KLT['name']} is not a Linux target — use mkguest.sh", file=sys.stderr)
        return 1
    if not LD.exists():
        print(f"!! {LD} missing — run 'make build/klepton-ld' first", file=sys.stderr)
        return 1
    if not LIST.exists():
        print(f"!! {LIST} missing — record it with a host run under KL_LX_LOADLOG "
              f"(see this file's header)", file=sys.stderr)
        return 1
    items = {}
    for line in LIST.read_text().splitlines():
        if "\t" in line:
            logged, guest = line.split("\t", 1)
            items[guest] = logged
    OUT.mkdir(parents=True, exist_ok=True)
    names, failed = [], 0
    with concurrent.futures.ThreadPoolExecutor(int(os.environ.get("JOBS", "8"))) as ex:
        for fw, note in ex.map(build, [(v, k) for k, v in sorted(items.items())]):
            if note:
                print(note)
            if fw:
                names.append(fw)
            else:
                failed += 1
    names = sorted(set(names))
    # Frameworks nothing asks for any more would still be embedded if left:
    # the project is generated from LIBS, but a stale directory is a stale
    # answer waiting for the next reader.
    for xc in OUT.glob("*.xcframework"):
        if xc.name.removesuffix(".xcframework") not in names:
            shutil.rmtree(xc)
    (OUT / "LIBS").write_text("\n".join(names) + "\n")
    size = sum((OUT / f"{n}.xcframework/xros-arm64/{n}.framework/{n}").stat().st_size
               for n in names)
    if failed:
        print(f"!! {failed} librar{'y' if failed == 1 else 'ies'} could not be built",
              file=sys.stderr)
        return 1
    print(f"[mklxguest] {KLT['name']} done -> {OUT}/  ({len(names)} frameworks, "
          f"{size >> 20} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
