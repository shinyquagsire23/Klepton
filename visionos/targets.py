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
        # dlopens libunity, which dlopens the rest — but every one is embedded,
        # because kl_load_auto resolves a DT_NEEDED against Frameworks/ and does
        # not care who asked.
        #
        # DISCOVERED, not pinned (None asks the Makefile — see libs_for below).
        # It used to name five libraries, which was the whole of 1.28; 1.40
        # ships eleven translatable ones and the pinned list silently left
        # libOculusXRPlugin — the XR-SDK display provider, i.e. the entire
        # render path — out of the bundle. On device that is not a fallback but
        # a dead end: the ELF tree is deliberately not in the bundle, so a
        # library with no translation cannot load at all. Same lesson as
        # `4dc27b1`: the guest library set is a property of the APK.
        "libs":    None,
        "srcdir":  "beatsaber/lib/arm64-v8a",
        "tree":    "beatsaber",
        "apk":     "beatsaber.apk",
        "assets":  "beatsaber/assets",
        # Nothing in this guest reads a library as a FILE, so no ELF goes into
        # the container at all. See steamlink-vr's `qtplugins`.
        "qtplugins": "",
        "product": "Klepton",
        "display": "Klepton",
        "bundle":  "dev.klepton.app",
    },
    "steamlink-vr": {
        # BOTH front doors, because the app runs both: the 2D shell pairs in a
        # WindowGroup and hands off to the OpenXR half in an ImmersiveSpace, in
        # one process (PLANNING §11.9 — an app bundle cannot re-exec the way
        # `build/m_slink` does).
        #
        # Three groups, and the third is the one that is easy to leave out:
        #   libvrlink_scene   the VR door. ONE library — its DT_NEEDED is
        #                     entirely Android system libraries we shim.
        #   the shell chain   fourteen, dependencies first, off libshell's own
        #                     DT_NEEDED (runtime/kl_slink.c's CHAIN_SHELL).
        #   the Qt plugins    six, in NOBODY's DT_NEEDED. Qt dlopens them by
        #                     path at runtime — the platform QPA first, and
        #                     libshell aborts without it. A dlopen that finds no
        #                     translation falls through to the mmap ELF loader,
        #                     which is exactly the RWX-from-an-unsigned-file
        #                     shape AMFI exists to refuse.
        "libs":    "libvrlink_scene "
                   "libc++_shared libSDL3 libSDL3_image libSDL3_mixer libSDL3_ttf "
                   "libQt6Core_arm64-v8a libQt6Network_arm64-v8a libQt6Gui_arm64-v8a "
                   "libQt6Widgets_arm64-v8a libQt6Svg_arm64-v8a "
                   "libh264bitstream libhevcbitstream libsteamwebrtc "
                   "libshell_arm64-v8a "
                   "libplugins_platforms_qvirtual_arm64-v8a "
                   "libplugins_iconengines_qsvgicon_arm64-v8a "
                   "libplugins_imageformats_qgif_arm64-v8a "
                   "libplugins_imageformats_qico_arm64-v8a "
                   "libplugins_imageformats_qjpeg_arm64-v8a "
                   "libplugins_imageformats_qsvg_arm64-v8a",
        "srcdir":  "steamlink-vr/lib/arm64-v8a",
        "tree":    "steamlink-vr",
        "apk":     "steamlink-vr.apk",
        "assets":  "steamlink-vr/assets",
        # The six plugin .so files ALSO go into the container, as ELF, and not
        # as a loader path — kl_load_auto still resolves each of them to its
        # signed framework by basename, so nothing maps guest text from here.
        #
        # **Qt reads a plugin as a FILE before it will load it.** libQt6Core's
        # search is a glob (`libplugins_%1_*.so`), so it lists the directory and
        # then parses each candidate's ELF metadata for the IID and the Qt
        # version. A directory of names the loader resolves is enough for
        # everything else in this project and is not enough for that: with no
        # real files nothing is ever a candidate, and libshell aborts with
        # `Could not find the Qt platform plugin "virtual"`.
        #
        # Six files, 544 KB — not the whole 75 MB tree, because these are the
        # only libraries anything reads rather than loads.
        "qtplugins": "steamlink-vr/lib/arm64-v8a",
        "product": "KleptonSteamLink",
        "display": "Klepton Steam Link",
        "bundle":  "dev.klepton.steamlink",
    },
}

DEFAULT = "beatsaber"


# `"libs": None` means "whatever this tree would translate for that srcdir",
# which the Makefile already decides — the .so files that are really ELF, minus
# the ones we REPLACE (libOVRPlugin, libovrplatformloader, libvrapi) and the
# ones that are not part of the application (libfrda, libscript). Asking it
# rather than restating it keeps one answer: a second copy of those rules here
# would drift on the next guest, and the failure mode is a bundle missing a
# library, which on device is a dlopen that cannot fall back to anything.
#
# It is a hard failure rather than a fallback to a pinned list, for the same
# reason: a quietly incomplete bundle is worse than a build that stops.
def libs_for(srcdir):
    import subprocess, os
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    out = subprocess.run(["make", "-s", "guestlibs-list", f"LIBS={srcdir}"],
                         cwd=root, capture_output=True, text=True)
    libs = out.stdout.strip()
    if out.returncode != 0 or not libs:
        print(f"!! could not discover the guest libraries in {srcdir!r} "
              f"(make guestlibs-list said: {out.stderr.strip() or 'nothing'})",
              file=sys.stderr)
        sys.exit(1)
    return libs


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    if t["libs"] is None:
        t["libs"] = libs_for(t["srcdir"])
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
