#!/usr/bin/env python3
"""Which guest a visionOS build is for — one table, three consumers.

`mkguest.sh`, `stage_assets.sh`, `run.sh` and `gen_xcodeproj.py` all need the
same handful of facts about a target, and until there was a second one they each
carried their own copy as Beat Saber literals. A table in four places is a table
that disagrees with itself, and the way that failure presents is the worst kind:
the build succeeds, the app installs, and the guest is described wrongly to
itself.

So this is the table, and the shells read it the same way the generator does:

    python3 targets.py <target> <field>      # one field, bare
    python3 targets.py <target>              # every field, as shell assignments
    python3 targets.py --list

Two apps built from this tree must not collide, and a bundle ID separates none
of the things that would (PLANNING §11, "Next up"). So the target also decides
the PRODUCT name — which carries the .xcodeproj, the .app and the derived-data
directory with it — and the subdirectory the translated guest frameworks are
staged into. ANGLE is deliberately NOT per-target: it is the same renderer for
every guest and `mkangle.sh` writes it once, into Frameworks/ itself.
"""
import sys

TARGETS = {
    "beatsaber": {
        # Unity + IL2CPP + Oculus Mobile SDK. The chain is staged: libmain
        # dlopens libunity, which dlopens the rest — but all five are embedded,
        # because kl_load_auto resolves a DT_NEEDED against Frameworks/ and does
        # not care who asked.
        "libs":    "libmain lib_burst_generated libunityopus libunity libil2cpp",
        "srcdir":  "beatsaber/lib/arm64-v8a",
        "tree":    "beatsaber",
        "apk":     "beatsaber.apk",
        "assets":  "beatsaber/assets",
        "product": "Klepton",
        "display": "Klepton",
        "bundle":  "dev.klepton.app",
    },
    "steamlink-vr": {
        # The OpenXR front door, and ONE library: libvrlink_scene's DT_NEEDED is
        # entirely Android system libraries we shim (PLANNING §11.9). The 2D
        # shell's fourteen are deliberately absent — see kl_app.c's boot_steamlink
        # for why the shell is not reachable from the app yet.
        "libs":    "libvrlink_scene",
        "srcdir":  "steamlink-vr/lib/arm64-v8a",
        "tree":    "steamlink-vr",
        "apk":     "steamlink-vr.apk",
        "assets":  "steamlink-vr/assets",
        "product": "KleptonSteamLink",
        "display": "Klepton Steam Link",
        "bundle":  "dev.klepton.steamlink",
    },
}

DEFAULT = "beatsaber"


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    return t


def main(argv):
    if len(argv) > 1 and argv[1] == "--list":
        for k in sorted(TARGETS):
            print(k)
        return 0
    name = argv[1] if len(argv) > 1 and argv[1] else DEFAULT
    t = resolve(name)
    if len(argv) > 2:
        if argv[2] not in t:
            print(f"!! no field {argv[2]!r}", file=sys.stderr)
            return 1
        print(t[argv[2]])
        return 0
    # Shell-sourceable. Quoted because `libs` has spaces in it and an unquoted
    # eval would turn one assignment into a command.
    for k, v in t.items():
        print(f"KLT_{k.upper()}='{v}'")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
