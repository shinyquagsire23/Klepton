#!/usr/bin/env python3
"""Keep a game's pipeline list (visionos/pipelines/<target>.pipelines) in step with the game.

    pipeline_tour.py status <target> <game dir>     what of the list this copy of the game still has, and which of
                                                    its files are new or changed since the list was made
    pipeline_tour.py tour   <target> <game dir> [<scene> ...]
                                                    walk the game on this Mac and record its pipelines — all of it,
                                                    or the named scenes only; merge into the kept recording
    pipeline_tour.py update <target> <game dir>     status; where it says a walk is due: walk what is due, merge,
                                                    and write the list anew
    pipeline_tour.py list   <target> <game dir>     write the list anew from the kept recording

<game dir> is the folder the game's files lie in (AVP Play's downloads folder for the game).

A list is made from a RECORDING: the game run on this Mac with the pipeline recorder in it (build/m_boot_fz), taken
through its content by its own scene loader, called with one scene's name after another
(visionos/pipelines/<target>.tour.json says which method that is and where the names are read from). The recording carries the game's shader code and stays on this Mac, under
~/Library/Application Support/Klepton/pipelines/<target>/; what goes into the repository is the recipe made of it
(tools/unity_pipelines.py), which carries none.

A walk through a whole game takes hours, so `update` decides first what is due. `<target>.files.json` beside the
list remembers, for every file of the game the list was made from (and every bundle inside the APK and the OBB), a
checksum of the SHADERS in it. A file that was only packed anew has the same shaders and needs no walk.
  - no file has other shaders than then: nothing to do;
  - only files of single levels have (a file named `<x>common_scenes_<scene>.bundle`, or `<x>common_assets_.bundle`,
    which is all scenes of <x>): those scenes are walked;
  - a file that every level shares has: everything is walked.
The new recording is merged with the one kept on this Mac (what did not change keeps its entries) and the list is
written again.
"""
import hashlib
import json
import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
PIPES = os.path.join(ROOT, "visionos", "pipelines")
sys.path.insert(0, HERE)
import unity_pipelines as up  # noqa: E402


def keep_dir(target):
    d = os.path.expanduser(f"~/Library/Application Support/Klepton/pipelines/{target}")
    os.makedirs(d, exist_ok=True)
    return d


def shader_sets(game):
    """{file[:member]: checksum of the shader programs in it} for everything in the game's folder that has any."""
    sets = {}
    for where, code in up.programs(game):
        # Inside the APK or an OBB the member is the name: the archive's own name carries the build number and
        # would make everything in it look new with every version.
        kind = "apk:" if where["file"].lower().endswith(".apk") else "obb:"
        key = kind + where["member"] if where["member"] else where["file"]
        sets.setdefault(key, []).append(hashlib.sha256(code).hexdigest())
    return {k: hashlib.sha256("".join(sorted(v)).encode()).hexdigest() for k, v in sets.items()}


LEVEL = re.compile(r"(?:.*/)?([a-z0-9-]+common)_(assets_|scenes_([a-z0-9_]+))\.bundle$")


def scenes_of(game, changed):
    """(scenes to walk, files that every level shares) for a set of files whose shaders are new or other."""
    names = [n for n in os.listdir(game)] + [f"{z}:{m}" for z in os.listdir(game) if z.lower().endswith((".obb", ".apk"))
                                             for m in _members(os.path.join(game, z))]
    scenes, shared = set(), []
    for key in changed:
        m = LEVEL.match(key.split(":")[-1])
        if not m:
            shared.append(key)
        elif m.group(3):
            scenes.add(m.group(3))
        else:                                   # a level's assets: every scene of that level
            for n in names:
                k = LEVEL.match(n.split(":")[-1])
                if k and k.group(1) == m.group(1) and k.group(3):
                    scenes.add(k.group(3))
    return sorted(scenes), shared


def _members(path):
    import zipfile
    try:
        return zipfile.ZipFile(path).namelist()
    except (zipfile.BadZipFile, OSError):
        return []


def status(target, game, quiet=False):
    """(scenes to walk — None for all, [] for none —, report lines)"""
    recipe = os.path.join(PIPES, f"{target}.pipelines")
    if not os.path.exists(recipe):
        if not quiet:
            print(f"there is no list for {target} yet: a walk of everything is due")
        return None, []
    lines = []
    out = os.path.join(keep_dir(target), "status.foz")
    text = subprocess.run([sys.executable, os.path.join(HERE, "unity_pipelines.py"), "build", recipe, game, out],
                          capture_output=True, text=True).stdout.strip().splitlines()
    lines.append("modules: " + (text[-1].split(": ", 1)[-1] if text else "the list could not be rebuilt from this copy"))
    try:
        os.remove(out)
    except OSError:
        pass
    now = shader_sets(game)
    try:
        then = json.load(open(os.path.join(PIPES, f"{target}.files.json")))["shaders"]
    except (OSError, ValueError, KeyError):
        then = None
    if then is None:
        lines.append("files: no record of the shaders the list was made from — a walk of everything is due")
        due = None
    else:
        new = sorted(k for k in now if k not in then)
        other = sorted(k for k in now if k in then and now[k] != then[k])
        lines.append(f"files with shaders: {len(new)} new, {len(other)} with other shaders than then, "
                     f"{len(now) - len(new) - len(other)} as they were")
        for k in new + other:
            lines.append(f"  {'new  ' if k in new else 'other'} {k}")
        scenes, shared = scenes_of(game, new + other)
        if shared:
            lines.append("shaders that every level shares are among them: a walk of everything is due")
            due = None
        elif scenes:
            lines.append(f"a walk of {len(scenes)} scene(s) is due: " + " ".join(scenes))
            due = scenes
        else:
            lines.append("the list fits this copy of the game; nothing to do")
            due = []
    if not quiet:
        print("\n".join(lines))
    return due, lines


def _files_dir(target, game, conf):
    """The game's data as the game expects to find it, made of links: nothing is copied and nothing of the game's is
    written to (it writes its own settings and saves beside them)."""
    files = os.path.join(keep_dir(target), "files")
    obb = os.path.join(files, "obb")
    loose = os.path.join(files, "Android", "obb", conf["package"])
    for d in (obb, loose):
        os.makedirs(d, exist_ok=True)
        for old in os.listdir(d):
            if os.path.islink(os.path.join(d, old)):
                os.remove(os.path.join(d, old))
    for name in sorted(os.listdir(game)):
        path = os.path.join(game, name)
        if name.lower().endswith(".obb"):
            os.symlink(path, os.path.join(obb, name))
        elif name.lower().endswith(".bundle"):
            os.symlink(path, os.path.join(loose, name))
    grants = os.path.expanduser(f"~/Library/Application Support/Klepton/userdata/{target}/klepton-entitlements.txt")
    if os.path.exists(grants):
        with open(grants, "rb") as a, open(os.path.join(files, "klepton-entitlements.txt"), "wb") as b:
            b.write(a.read())
    return files


def scene_names(target, conf, env, runner):
    """{scene as the files call it (lower case): the name the game loads it by}, from the game's own list of its
    courses — a short run that only reads that list. Kept beside the recording; read anew for every walk, because a
    new course is exactly what a walk is for."""
    e = dict(env)
    e.update({"KL_FRAMES": str(conf["from_frame"] + 200), "KL_PROBE_CALL_FROM": str(conf["from_frame"]),
              "KL_PROBE_KEEP": conf["names"] + "=_", "KL_PROBE_KEEP_FIELD": conf.get("names_field", "")})
    out = subprocess.run([runner, target], cwd=ROOT, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.DEVNULL).stdout.decode(errors="replace")
    names = {}
    for m in re.finditer(r"\[mprobe f\d+\] keep:\s+\d+\s+(\S+)\s+(\S+)\s+(?:kept|dropped)", out):
        names[m.group(2).lower()] = m.group(2)
    cache = os.path.join(keep_dir(target), "names.json")
    if names:
        json.dump(names, open(cache, "w"), indent=1, sort_keys=True)
    else:                                       # the list could not be read: what the last walk knew
        try:
            names = json.load(open(cache))
        except (OSError, ValueError):
            pass
    return names


def tour(target, game, scenes=None):
    """Walk the game — everything, or the named scenes — and merge what it recorded into the kept recording.

    Each scene is loaded by the game's own loader, called with the scene's name (`load` in <target>.tour.json), one
    after another, `frames_per_scene` apart. `before` are calls made first, where the game starts (Walkabout: into the
    Homestar Runner basement, which is part of its main menu)."""
    conf = json.load(open(os.path.join(PIPES, f"{target}.tour.json")))
    runner = os.path.join(ROOT, "build", "m_boot_fz")
    subprocess.run(["make", "-s", "build/m_boot_fz"], cwd=ROOT, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not os.path.isdir(os.path.join(ROOT, target)) or not os.path.exists(os.path.join(ROOT, f"{target}.apk")):
        raise SystemExit(f"{target}/ and {target}.apk are not in this toolchain copy — install the game once from it "
                         f"(avpplay job start {target} --build-only --toolchain {ROOT}); that unpacks them")
    files = _files_dir(target, game, conf)
    rec = os.path.join(keep_dir(target), "walk")
    os.makedirs(rec, exist_ok=True)
    old = os.path.join(rec, "recorded.foz")
    if os.path.exists(old):
        os.remove(old)                          # each walk records anew; the kept recording is the memory
    env = dict(os.environ)
    env.update({"KL_FILES_DIR": files, "KL_VK_FOZ_DIR": rec, "KL_NET_OFFLINE": "1", "KL_GLFB": "1", "KL_LIFECYCLE": "1",
                "KL_ALARM": str(conf.get("alarm_seconds", 14400))})
    names = scene_names(target, conf, env, runner)
    on_disk = sorted({m.group(3) for m in (LEVEL.match(n) for n in os.listdir(game)) if m and m.group(3)})
    wanted = [s.lower() for s in scenes] if scenes else on_disk
    nameless = [s for s in wanted if s not in names]
    if nameless:
        print("not in the game's own list of courses, so there is no name to load them by: " + " ".join(nameless))
    walk = [s for s in wanted if s in names]
    if not walk:
        print("nothing to walk")
        return None
    per = int(conf.get("frames_per_scene", 3000))
    frame = int(conf["from_frame"])
    steps = []
    for call in conf.get("before", []):
        steps.append(f"{frame}:{call}")
        frame += per
    for s in walk:
        steps.append(f'{frame}:{conf["load"]}("{names[s]}")')
        frame += per
    # Content that is entered from objects in the start scene rather than by a scene's name (Walkabout: the 36
    # launchers in the Homestar Runner basement — its mini-games, and its notes that lead to places on the courses):
    # `call` on each of them in turn, `then` to come back. Part of a walk of everything only.
    objects = conf.get("objects") if not scenes else None
    if objects:
        steps.append(f'{frame}:{objects["then"]}')
        frame += per
        for n in range(int(objects["count"])):
            steps.append(f'{frame}:{objects["call"]}#{n}')
            frame += 2 * per
            steps.append(f'{frame}:{objects["then"]}')
            frame += per
    env.update({"KL_PROBE_SCRIPT": ";".join(steps), "KL_FRAMES": str(frame), "KL_TRACE_FS": "1"})
    log = os.path.join(keep_dir(target), "walk.log")
    print(f"walking {target}: {len(walk)} scene(s) — the log is {log}", flush=True)
    t0 = time.time()
    with open(log, "wb") as f:
        rc = subprocess.run([runner, target], cwd=ROOT, env=env, stdout=f, stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL).returncode
    recorded = os.path.join(rec, "recorded.foz")
    size = os.path.getsize(recorded) if os.path.exists(recorded) else 0
    print(f"walk ended after {(time.time() - t0) / 60:.0f} min (exit {rc}); recording {size} bytes")
    # A scene whose file the game never opened is one it did not enter, and one the list does not have.
    opened = set()
    for line in open(log, errors="replace"):
        if line.startswith("[fs] open(") and "_scenes_" in line:
            m = LEVEL.match(line.split('"')[1])
            if m and m.group(3):
                opened.add(m.group(3))
    missed = [s for s in walk if s not in opened]
    print(f"entered {len(walk) - len(missed)} of {len(walk)}" + (": not " + " ".join(missed) if missed else ""))
    for line in open(log, errors="replace"):
        if "[mprobe" in line and (" threw " in line or "there are" in line or "call: no " in line):
            print("  " + line.split("] ", 1)[-1].strip()[:150])
    if not size:
        return None
    keep = os.path.join(keep_dir(target), "recorded.foz")
    merged = keep + ".new"
    up.merge(merged, [recorded] + ([keep] if os.path.exists(keep) else []))
    os.replace(merged, keep)
    return keep


def write_list(target, game):
    keep = os.path.join(keep_dir(target), "recorded.foz")
    recipe = os.path.join(PIPES, f"{target}.pipelines")
    if up.recipe(keep, game, recipe + ".new") != 0 or up.check(recipe + ".new") != 0:
        if os.path.exists(recipe + ".new"):
            os.remove(recipe + ".new")
        return 1
    os.replace(recipe + ".new", recipe)
    json.dump({"shaders": shader_sets(game)}, open(os.path.join(PIPES, f"{target}.files.json"), "w"), indent=1, sort_keys=True)
    return 0


def update(target, game):
    due, _ = status(target, game)
    if due == []:
        return 0
    if not tour(target, game, due):
        print("the walk recorded nothing — the list is left as it is")
        return 1
    if write_list(target, game) != 0:
        return 1
    status(target, game)
    return 0


def main(argv):
    if len(argv) == 4 and argv[1] == "status":
        status(argv[2], argv[3])
        return 0
    if len(argv) >= 4 and argv[1] == "tour":
        return 0 if tour(argv[2], argv[3], [x.lower() for x in argv[4:]] or None) else 1
    if len(argv) == 4 and argv[1] == "list":
        return write_list(argv[2], argv[3])
    if len(argv) == 4 and argv[1] == "update":
        return update(argv[2], argv[3])
    print(__doc__, file=sys.stderr)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))
