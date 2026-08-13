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
    python3 targets.py --c-table             # ...and the RUNTIME's copy, generated

The last one is the fifth consumer and the reason this file grew `entry` and
`kind`: the runtime needs the same table (which tree, which APK, which library
the chain starts at, which boot sequence), and it needed it twice — once in
`visionos/Sources/kl_app.c` for the app and once in `mains/m_boot.c` for the
host driver. Two hand-written C copies of a Python table is the exact failure
this file's header warns about, so `make targets` generates
`runtime/kl_target_table.h` from here instead. It is committed, like
`kl_libc_table.h` and `kl_jni_slots.h`, so a build never depends on Python.

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
        # The library the chain STARTS at — libmain's JNI_OnLoad is the whole
        # entry point for a Unity title (CLAUDE.md, "Facts worth not
        # rediscovering") — and, on device, the one whose translation being
        # present proves this target's guest was embedded rather than the other
        # app's.
        "entry":   "libmain",
        # Which boot sequence runs. "unity" is libmain -> NativeLoader.load ->
        # libunity -> the Android lifecycle; "steamlink" is kl_slink's doors.
        "kind":    "unity",
        "product": "Klepton",
        "display": "Klepton",
    },
    "superhot": {
        # SUPERHOT VR — Unity + IL2CPP + the Oculus Mobile SDK, i.e. the same
        # shape as Beat Saber and the reason it is the third target: it shares
        # the entire Unity path and exercises it against an APK nothing here was
        # written against. Its own manifest declares the same
        # com.unity3d.player.UnityPlayerActivity and the same VR intent
        # category, so nothing about the front door differs.
        #
        # Not a split-binary build: the assets are in the APK, so there is no
        # OBB to stage (stage_assets.sh treats one as present-or-absent).
        "libs":    None,
        "srcdir":  "superhot/lib/arm64-v8a",
        "tree":    "superhot",
        "apk":     "superhot.apk",
        "assets":  "superhot/assets",
        "qtplugins": "",
        "entry":   "libmain",
        "kind":    "unity",
        "product": "KleptonSuperhot",
        "display": "Klepton SUPERHOT",
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
        # The VR door's library. The shell's fourteen load on top of it in the
        # same process; this is the one whose translation being in the bundle
        # says the Steam Link guest was embedded.
        "entry":   "libvrlink_scene",
        "kind":    "steamlink",
        "product": "KleptonSteamLink",
        "display": "Klepton Steam Link",
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


# A bundle id is GLOBAL, so the one in the table is not usable as it stands.
#
# An App ID can be registered to exactly one team, and automatic signing tries to
# register one on the first build. So the second person to build this tree does
# not get a warning, they get a signing failure that names neither the cause nor
# the fix:
#
#   error: Failed Registering Bundle Identifier: the app identifier
#          "dev.klepton.app" cannot be registered to your development team
#
# ...and because both memory entitlements need an EXPLICIT App ID, that failure
# takes them down with it — which is the thing that stopped the
# loading-transition kills on device (see ENTITLEMENTS in gen_xcodeproj.py). One
# person's registration is enough to block everyone else's build of the same
# commit.
#
# So the account building it supplies the front of the id. $USER is the right key
# because it is per-machine-account, always set in a login shell, and needs no
# configuration at all to be different for different people.
#
# The rest is DERIVED from the target name rather than stored per target:
#
#     <user>.dev.klepton.target.<target>
#
# One shape for every target, so a new one needs no id invented for it and the
# table cannot hold an id that disagrees with the key above it. (The three that
# used to be there did exactly that — `dev.klepton.app`, `dev.klepton.steamlink`
# and `dev.klepton.target.superhot` were three conventions for three targets.)
#
# Sanitised, because a bundle id may hold only alphanumerics, hyphens and
# periods: anything else becomes a hyphen, and a scope that sanitises to nothing
# (or an unset USER, as in some CI) leaves the id unscoped rather than emitting
# a leading dot. KLEPTON_BUNDLE_SCOPE overrides the key; KLEPTON_BUNDLE_ID still
# overrides the whole id, everywhere, and is how an ALREADY-INSTALLED app's
# identity is kept — changing this orphans the container the assets were staged
# into, so the first build after it re-stages (run.sh's stamp is keyed on the id,
# which is what makes that automatic rather than a silent empty container).
BUNDLE_PREFIX = "dev.klepton.target"


def bundle_for(name):
    import os, re
    scope = os.environ.get("KLEPTON_BUNDLE_SCOPE", os.environ.get("USER", ""))
    scope = re.sub(r"[^A-Za-z0-9-]", "-", scope).strip("-").lower()
    ident = re.sub(r"[^A-Za-z0-9.-]", "-", f"{BUNDLE_PREFIX}.{name}")
    return f"{scope}.{ident}" if scope else ident


def resolve(name):
    if name not in TARGETS:
        print(f"!! unknown target {name!r} — one of: {', '.join(sorted(TARGETS))}",
              file=sys.stderr)
        sys.exit(1)
    t = dict(TARGETS[name])
    t["name"] = name
    t["bundle"] = bundle_for(name)
    if t["libs"] is None:
        t["libs"] = libs_for(t["srcdir"])
    return t


# The runtime's copy of the table, as an X-macro list. Only the fields C needs
# are here: the visionOS product/bundle/display are the BUILD's business and the
# app already knows which one it is by the time it runs, and `libs` is discovered
# per build rather than being a property anything at runtime should believe.
#
# The DEFAULT is emitted too, so `build/m_boot` with no argument and an app built
# with no -DKL_TARGET_DEFAULT agree about which guest that means.
def c_table():
    out = [
        "// GENERATED by visionos/targets.py — do not edit. Regenerate: make targets",
        "//",
        "// The target table, as the runtime sees it. visionos/targets.py is the",
        "// authority (it also carries the bundle id, product and display name, which",
        "// only the build needs); this is its C half, generated so that the two",
        "// cannot disagree about which tree, which APK or which boot sequence a",
        "// target means. See runtime/kl_target.h for the struct these expand into.",
        "",
        "// KL_TARGET_ROW(name, tree, apk, assets, libdir, entry_lib, kind)",
        "#ifndef KL_TARGET_ROW",
        "#error \"include this through runtime/kl_target.c\"",
        "#endif",
        "",
    ]
    for name in sorted(TARGETS):
        t = TARGETS[name]
        kind = "KL_GUEST_" + t["kind"].upper()
        out.append('KL_TARGET_ROW("%s", "%s", "%s", "%s", "%s", "%s", %s)'
                   % (name, t["tree"], t["apk"], t["assets"], t["srcdir"],
                      t["entry"], kind))
    out += ["", '#define KL_TARGET_DEFAULT_NAME "%s"' % DEFAULT, ""]
    return "\n".join(out)


def main(argv):
    if len(argv) > 1 and argv[1] == "--list":
        for k in sorted(TARGETS):
            print(k)
        return 0
    if len(argv) > 1 and argv[1] == "--c-table":
        print(c_table())
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
