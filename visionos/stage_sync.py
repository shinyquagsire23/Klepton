#!/usr/bin/env python3
"""Stage a Linux target's files into the app's Documents container.

stage_assets.sh's counterpart for a target whose guest is a Linux process (kind
"linux" in targets.py). That script replaces what is there, which is right for
an APK and its assets and unusable for a 70 GB game: this one is incremental,
and can stage the part a first run needs before the rest.

    ./stage_sync.py                     # the first physical Vision Pro, phase "start"
    ./stage_sync.py --phase all         # everything (tens of GB; resumable)
    ./stage_sync.py --phase base        # no game content at all: the sysroot and
                                        # the binaries, for a small test program
    ./stage_sync.py --dry-run           # build the mirror, say what it holds
    ./stage_sync.py --device <udid>

What lands on the device is <container>/Documents/lx — the tree
runtime/guest/kl_linuxproc.h describes:

    lx/sysroot   the Steam Linux Runtime's files/
    lx/game      the game's install directory
    lx/xrizer    OpenVR-over-OpenXR (bin/linuxarm64/vrclient.so)
    lx/links.tsv every symbolic link of those trees

How:

  * A MIRROR of exactly what should be on the device is built under
    build/lxstage/<target>/lx from hard links, so it costs no space and no
    copying on this machine, and `devicectl device copy to` is pointed at it.
    devicectl skips files that have not changed, which is the whole of the
    incremental part; a run that was interrupted is resumed by running it again.

    A hard link cannot leave its volume. With the game on another disk than
    this checkout every "link" is a copy — 70 GB of them, onto whichever disk
    the checkout is on (a 2 TB system disk with 79 GB free was filled this way,
    2026-10-09). KL_LX_MIRROR=<directory> puts the mirror somewhere else: a
    directory of its own on the game's volume, which this script empties and
    fills, and nothing else may live in.

  * No symbolic links are staged. A sysroot is full of them and devicectl
    copies a link as a link, pointing wherever it pointed on this Mac. They are
    written to links.tsv instead and the app makes them at boot.

  * An ELF file is staged only if the process loads it (lx/<target>.loaded, the
    list mklxguest.py builds the frameworks from). The runtime needs the file —
    ld.so opens it, and its content names the translation — but a library with
    no translation in the bundle cannot be loaded, so staging it buys nothing.
    These are data on the device: nothing is ever executed out of the
    container.

  * Phase "start" leaves out the game's numbered pak01 archives and maps that a
    recorded run did not open (lx/<target>.opened, from KL_LX_OPENLOG). The
    engine opens an archive when it first reads from it, so a run that goes
    further than the recorded one stops at a named missing file — and phase
    "all" is what makes that impossible.
"""
import argparse
import json
import os
import pathlib
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
sys.path.insert(0, str(HERE))
import targets as targets_mod  # noqa: E402

SYSROOT = pathlib.Path(os.environ.get("KL_LX_SYSROOT", ROOT.parent / "steamrt/root/files"))
GAME = pathlib.Path(os.environ.get("KL_LX_GAME", ROOT.parent / "alyx/game"))
XRIZER = pathlib.Path(os.environ.get("KL_LX_XRIZER", ROOT.parent / "guestbuild/xrizer"))
# Guest prefix -> (host tree, where it lands under lx/). tools/lx_alyx.sh's mounts.
MOUNTS = [("/opt/xrizer/", XRIZER, "xrizer"), ("/game/", GAME, "game"),
          ("/usr/", SYSROOT, "sysroot"), ("/lib/", SYSROOT / "lib", "sysroot/lib"),
          ("/bin/", SYSROOT / "bin", "sysroot/bin"), ("/etc/", SYSROOT / "etc", "sysroot/etc")]

# Never read by a running program.
SYSROOT_SKIP = ("include", "src", "share/doc", "share/man", "share/info", "share/locale",
                "share/lintian", "share/bash-completion", "share/zsh")
# The game ships x86-64 binaries beside the ARM64 ones.
GAME_SKIP = ("bin/linuxsteamrt64", "bin/win64", "hlvr/bin/linuxsteamrt64", "hlvr/bin/win64",
             "core/bin/linuxsteamrt64", "core/bin/win64")
# What the game WRITES into its own tree: its settings and saves. The copies on
# this machine are whatever a host run last left there, and staging them would
# replace the settings and save games the player made on the headset. The
# fidelity table is the app's (KleptonAlyx.swift writes it at every start).
# Not the depot's own files among them: the *_default*.vcfg the engine builds
# the player's settings from (without them it calls the content corrupt and
# stops), and deckard/dummy.json, which is what makes that directory exist.
GAME_OWN = re.compile(r"^hlvr/(cfg/(machine|user)(?![^/]*_default)[^/]*\.vcfg"
                      r"|cfg/deckard/(?!dummy\.json$).+|save/.+|console\.log"
                      r"|vr_fidelity_levels_override\.csv)$")
# What phase "start" holds back unless the recorded run opened it.
LAZY = re.compile(r"^hlvr/(pak01_\d+\.vpk|maps/.+|hlvr_addons/.+)$")


def launcher_links(game):
    """The links the game's launch script makes in bin/linuxsteamrtarm64.

    A Steam depot cannot carry links, so it ships zero-byte placeholders
    (libpango-1.0.so and friends) and hlvr.sh replaces them at launch; nothing
    runs that script here. A tree that went through tools/lx_alyx_prepare.sh has
    the real links already; one fetched straight from Steam has only the
    placeholders, and ld.so stops at them ("file too short"). Same list as
    lx_alyx_prepare.sh: read out of hlvr.sh, so it follows the depot.
    """
    try:
        text = (game / "hlvr.sh").read_text(errors="replace")
    except OSError:
        return {}
    m = re.search(r"linuxsteamrtarm64\)(.*?);;", text, re.S)
    if not m:
        return {}
    return {f"bin/linuxsteamrtarm64/{name}": target
            for name, target in re.findall(r'"([^":]+):([^":]+)"', m.group(1))}


def staged_path(guest):
    for prefix, _, under in MOUNTS:
        if guest.startswith(prefix):
            return f"{under}/{guest[len(prefix):]}"
    return None


def read_lists(name):
    loaded, opened = set(), set()
    p = HERE / "lx" / f"{name}.loaded"
    if p.exists():
        for line in p.read_text().splitlines():
            if "\t" in line:
                s = staged_path(line.split("\t", 1)[1])
                if s:
                    loaded.add(s)
    p = HERE / "lx" / f"{name}.opened"
    if p.exists():
        for line in p.read_text().splitlines():
            s = staged_path(line.strip())
            if s:
                opened.add(s)
    return loaded, opened


def is_elf(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) == b"\x7fELF"
    except OSError:
        return False


def link(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try:
        os.link(src, dst)
    except OSError:
        shutil.copy2(src, dst)          # another volume: a real copy, mtime kept


def walk(tree, under, skip, want, mirror, links, stats):
    """Mirror `tree` at mirror/<under>, recording links instead of making them."""
    if not tree.is_dir():
        return
    for dirpath, dirnames, filenames in os.walk(tree):
        rel_dir = os.path.relpath(dirpath, tree)
        rel_dir = "" if rel_dir == "." else rel_dir
        keep = []
        for d in dirnames:
            rel = f"{rel_dir}/{d}" if rel_dir else d
            full = os.path.join(dirpath, d)
            if os.path.islink(full):
                links.append((f"{under}/{rel}", os.readlink(full)))
            elif not rel.startswith(skip):
                keep.append(d)
        dirnames[:] = keep
        for fn in filenames:
            rel = f"{rel_dir}/{fn}" if rel_dir else fn
            full = os.path.join(dirpath, fn)
            if os.path.islink(full):
                links.append((f"{under}/{rel}", os.readlink(full)))
                continue
            if not os.path.isfile(full) or rel.startswith(skip):
                continue
            verdict = want(f"{under}/{rel}", rel, full)
            if verdict is not True:
                stats[verdict] = stats.get(verdict, 0) + 1
                continue
            link(full, mirror / under / rel)
            stats["files"] = stats.get("files", 0) + 1
            stats["bytes"] = stats.get("bytes", 0) + os.path.getsize(full)


def build_mirror(name, phase, mirror):
    loaded, opened = read_lists(name)
    if not loaded:
        print(f"!! lx/{name}.loaded is missing or empty — no library would be staged",
              file=sys.stderr)
        sys.exit(1)
    shutil.rmtree(mirror, ignore_errors=True)
    mirror.mkdir(parents=True)
    links, stats = [], {}
    # Placeholders and missing links of the launch script: links.tsv makes them.
    # A target may itself be one of these links (libpango-1.0.so -> .so.0).
    wanted = launcher_links(GAME)
    placeholders = {}
    for rel, target in wanted.items():
        full = GAME / rel
        if full.is_symlink():
            continue                    # a prepared tree: walk() records it
        if full.exists() and not (full.is_file() and full.stat().st_size == 0):
            continue                    # something real is there
        to = full.parent / target
        if not (to.exists() or to.is_symlink() or f"{os.path.dirname(rel)}/{target}" in wanted):
            continue
        placeholders[rel] = target

    def want_sys(staged, rel, full):
        if is_elf(full) and staged not in loaded:
            return "ELF files nothing loads"
        return True

    def want_game(staged, rel, full):
        if rel in placeholders:
            return "placeholders of the launch script, made as links"
        if is_elf(full) and staged not in loaded:
            return "ELF files nothing loads"
        if GAME_OWN.match(rel):
            return "settings and saves, which are the device's own"
        if phase == "base" and "/bin/" not in f"/{rel}":
            return "game content (phase base)"
        if phase == "start" and LAZY.match(rel) and staged not in opened:
            return "archives and maps the recorded run did not open (phase start)"
        return True

    walk(SYSROOT, "sysroot", SYSROOT_SKIP, want_sys, mirror, links, stats)
    walk(GAME, "game", GAME_SKIP, want_game, mirror, links, stats)
    walk(XRIZER / "bin", "xrizer/bin", (), lambda *a: True, mirror, links, stats)
    links += [(f"game/{rel}", target) for rel, target in placeholders.items()]
    with open(mirror / "links.tsv", "w") as f:
        for at, target in sorted(links):
            f.write(f"{at}\t{target}\n")
    missing = sorted(s for s in loaded if not (mirror / s).is_file())
    if phase != "base":
        pipeline_list(name, mirror, stats)
    return stats, len(links), missing


def pipeline_list(name, mirror, stats):
    """The game's Vulkan pipelines, for the app to compile before the game starts.

    lx/<name>.pipelines is a recipe: a pipeline list with every shader module
    replaced by where it is in the game's own shader packs (Glovebox's format
    and, for Alyx, Glovebox's list — github.com/glovebox-vr/glovebox, MIT).
    tools/alyx_pipelines.py makes the list of it from this copy of the game,
    which takes a quarter of a minute, so the result is kept beside the build
    products under a name that changes with the recipe and with the packs. It
    goes to lx/klfz/<name>.foz, where the app looks (runtime/gfx/
    kl_fossilize.cpp), with lx/<name>-device-info.json beside it: how the game
    makes its Vulkan device, for a first start that has no record of its own.

    A game without the list is a game that compiles as it goes, as before, so
    nothing here stops the staging.
    """
    recipe = HERE / "lx" / f"{name}.pipelines"
    tool = ROOT / "tools" / "alyx_pipelines.py"
    if not recipe.is_file() or not tool.is_file():
        return
    packs = sorted(p for d in ("core", "hlvr") for p in (GAME / d).glob("shaders_vulkan_*.vpk"))
    if not packs:
        print(f"[stage] no pipeline list: {GAME} has no shader packs")
        return
    h = hashlib.sha256()
    for f in (recipe, tool):
        h.update(f.read_bytes())
    for p in packs:
        st = p.stat()
        h.update(f"{p.relative_to(GAME)}:{st.st_size}:{st.st_mtime_ns};".encode())
    keep = HERE / "build" / "pipelines"
    made = keep / f"{name}-{h.hexdigest()[:16]}.foz"
    if not made.is_file():
        keep.mkdir(parents=True, exist_ok=True)
        for old in keep.glob(f"{name}-*.foz*"):
            old.unlink()
        t0 = time.time()
        r = subprocess.run([sys.executable, "-I", str(tool), "build", str(recipe), str(GAME), str(made)],
                           capture_output=True, text=True)
        if r.returncode != 0 or not made.is_file():
            why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or ["no reason given"])[-1]
            print(f"[stage] no pipeline list: {why[:200]} — the game compiles its shaders as it goes")
            return
        print(f"[stage] pipeline list built from the game's shader packs "
              f"({made.stat().st_size / 2**20:.1f} MB, {time.time() - t0:.0f} s)")
    (mirror / "klfz").mkdir(parents=True, exist_ok=True)
    link(made, mirror / "klfz" / f"{name}.foz")
    n, size = 1, made.stat().st_size
    info = HERE / "lx" / f"{name}-device-info.json"
    if info.is_file():
        shutil.copyfile(info, mirror / "klfz" / "device-info.default.json")
        n, size = n + 1, size + info.stat().st_size
    stats["files"] = stats.get("files", 0) + n
    stats["bytes"] = stats.get("bytes", 0) + size


def find_device():
    with tempfile.NamedTemporaryFile(suffix=".json") as t:
        subprocess.run(["xcrun", "devicectl", "list", "devices", "--json-output", t.name],
                       capture_output=True)
        try:
            devs = json.load(open(t.name))["result"]["devices"]
        except Exception:
            return None
    phys = [d for d in devs
            if d.get("hardwareProperties", {}).get("reality") == "physical"
            and d.get("hardwareProperties", {}).get("platform") == "visionOS"]
    phys.sort(key=lambda d: d.get("connectionProperties", {}).get("tunnelState") != "connected")
    return phys[0]["identifier"] if phys else None


def copy(device, bundle, source, destination):
    return subprocess.run(["xcrun", "devicectl", "device", "copy", "to", "--device", device,
                           "--domain-type", "appDataContainer", "--domain-identifier", bundle,
                           "--source", str(source), "--destination", destination]).returncode


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--phase", choices=("base", "start", "all"), default="start")
    ap.add_argument("--device")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    klt = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or "alyx")
    if klt["kind"] != "linux":
        print(f"!! {klt['name']} is not a Linux target — use stage_assets.sh", file=sys.stderr)
        return 1
    bundle = os.environ.get("KLEPTON_BUNDLE_ID", klt["bundle"])
    mirror = HERE / "build" / "lxstage" / klt["name"] / "lx"
    if os.environ.get("KL_LX_MIRROR"):
        mirror = pathlib.Path(os.environ["KL_LX_MIRROR"])

    stats, nlinks, missing = build_mirror(klt["name"], args.phase, mirror)
    print(f"[stage] phase {args.phase}: {stats.get('files', 0)} files, "
          f"{stats.get('bytes', 0) / 2**30:.1f} GB, {nlinks} links in links.tsv")
    for why, n in sorted(stats.items()):
        if why not in ("files", "bytes"):
            print(f"[stage]   left out: {n} {why}")
    for m in missing:
        print(f"[stage] !! {m} is loaded by the process and is not in the mirror")
    if args.dry_run:
        print(f"[stage] dry run — the mirror is at {mirror}")
        return 0

    device = args.device or os.environ.get("KLEPTON_DEVICE") or find_device()
    if not device:
        print("!! no physical Vision Pro — pair it in Xcode, or pass --device", file=sys.stderr)
        return 1
    # Small things first and one directory per call, so that an interrupted run
    # has finished whole parts and the long one is last.
    parts = [p for p in ("links.tsv", "klfz", "xrizer", "sysroot") if (mirror / p).exists()]
    game = mirror / "game"
    if game.is_dir():
        parts += sorted(f"game/{c.name}" for c in game.iterdir() if c.name != "hlvr")
        if (game / "hlvr").is_dir():
            parts.append("game/hlvr")
    t0 = time.time()
    for part in parts:
        print(f"[stage] -> Documents/lx/{part}", flush=True)
        if copy(device, bundle, mirror / part, f"Documents/lx/{part}") != 0:
            print(f"!! staging {part} failed — run this again to resume", file=sys.stderr)
            return 1
    print(f"[stage] done in {time.time() - t0:.0f} s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
