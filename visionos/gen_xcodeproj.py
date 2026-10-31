#!/usr/bin/env python3
"""Generate the visionOS host app's .xcodeproj with no Xcode GUI needed.

Grown from spikes/device-probe/gen_xcodeproj.py, but data-driven: the guest
libraries are a list, not seven hand-written object ids each. That list — and
the product name, bundle id and display name that go with it — now comes from
visionos/targets.py, keyed on KLEPTON_TARGET. There are two guests, and two apps
built from one tree must not collide: a bundle ID separates the INSTALLED apps
and nothing about their build outputs, so the target carries the product name
(hence the .xcodeproj, the .app and the derived-data directory) and the
Frameworks/ subdirectory its translations were staged into.

What the app links, and why it comes from three places:

  Klepton.xcframework   the C runtime, built by `make xros` (both slices).
                        Static, so it is inside the app binary and carries no
                        load-time cost of its own.
  <guest>.xcframework   the five klepton-ld translations, built by mkguest.sh.
                        Embedded and signed-on-copy, NOT linked — nothing
                        references their symbols; the runtime dlopens them by
                        path. Linking them would make dyld resolve an exports
                        trie that klepton-ld deliberately does not emit.
  ANGLE_*.xcframework   ANGLE retargeted to visionOS, built by mkangle.sh.
                        Embedded the same way and for the same reason: kl_glfb
                        dlopens libEGL/libGLESv2 by path, and linking a renderer
                        the app never names would only make dyld load it on
                        every run, including the ones that stay on the null
                        driver.
  Sources/              the Swift app layer and kl_app.c.
"""
import os, pathlib, subprocess, sys, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import targets as targets_mod

HERE = os.path.dirname(os.path.abspath(__file__))
SC2_PROFILE = os.environ.get('SC2_GAME_PROFILE', '1.40.8')
if SC2_PROFILE not in ('1.40.8', '1.45.3'):
    raise SystemExit('Unknown Scotland2 game profile')
KLT = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or targets_mod.DEFAULT)
# The PRODUCT name, which is also the .xcodeproj's, the .app's and the scheme's.
# The Swift sources are still Klepton*.swift whatever the product is called —
# they are the app layer, shared by every target, and renaming them per build
# would be five file copies to say one thing.
NAME = KLT["product"]
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", KLT["bundle"])

# Whether this is a TEST build — a copy beside the real game, for trying
# something on the headset — and not what a player has (KL_TEST_BUILD below,
# KLTestBuild in Info.plist, KleptonTestBuild in KleptonTestBuild.swift). Only
# what a tester's command line sets counts: a name of its own
# (KLEPTON_DISPLAY_SUFFIX / _OVERRIDE — `avpplay install --display-suffix`), a
# bundle id ending in "-test" or ".test" in any case (regex [-.]test$; testing.md's
# convention for a test copy), KL_TEST_BUILD among KLEPTON_TEST_ON, or
# KLEPTON_TEST_BUILD=1. Deliberately NOT
# any bundle id that differs from the target's, nor a non-empty KLEPTON_TEST_ON:
# AVP Play sets the first for every install (a team's own prefix) and the
# second for a player who chose its experimental switches.
_bundle_test = re.search(r"[-.]test$", BUNDLE_ID or "", re.I) is not None and BUNDLE_ID != KLT["bundle"]
TEST_BUILD = (os.environ.get("KLEPTON_TEST_BUILD", "").strip().lower() in ("1", "true", "yes", "on")
              or bool(os.environ.get("KLEPTON_DISPLAY_SUFFIX", "").strip())
              or bool(os.environ.get("KLEPTON_DISPLAY_OVERRIDE", "").strip())
              or _bundle_test
              or "KL_TEST_BUILD" in re.split(r"[,\s]+", os.environ.get("KLEPTON_TEST_ON", "")))
if os.environ.get("SC2_BUILD_MODE") == "release":
    TEST_BUILD = False
if TEST_BUILD:
    print("gen_xcodeproj: test build (KLTestBuild=1): tester-only controls are in it", file=sys.stderr)

# Klepton.entitlements — the two memory capabilities (see that file for what
# they buy and why this app wants them). **On by default: they fixed the
# loading-transition kills on device, so a build without them is the unusual
# one.**
#
# KLEPTON_ENTITLEMENTS=0 detaches them, and the reason to keep that escape hatch
# is that this is an account setting as much as a build setting. Both keys need
# an EXPLICIT App ID with the capabilities enabled on it; the team's *wildcard*
# profile cannot carry capabilities at all, and attaching them to one does not
# degrade — it fails the build outright:
#
#   error: Provisioning profile "iOS Team Provisioning Profile: *" doesn't
#          support the Extended Virtual Addressing and Increased Memory Limit
#          capability.
#   error: Failed Registering Bundle Identifier: the app identifier
#          "…dev.klepton.target.beatsaber" cannot be registered to your
#          development team
#
# That pair of errors means the App ID is not set up, not that the entitlements
# are wrong. Enable both capabilities on it (Xcode > Settings > Accounts, or the
# developer portal) — or build with KLEPTON_ENTITLEMENTS=0 to get moving without
# them, at the cost of the memory headroom.
#
# The SECOND error has another cause worth telling apart, and it is why the id
# carries $USER (targets.py, bundle_for): an App ID belongs to exactly one team,
# so an UNSCOPED id is registered by whoever builds first and refused to everyone
# else — the same sentence, about a name already spoken for rather than about
# capabilities. If it appears with a scoped id, it really is the App ID setup.
ENTITLEMENTS = os.environ.get("KLEPTON_ENTITLEMENTS", "1") != "0"

# ...and the keys that are NOT self-service, each with its own escape hatch.
#
# The two memory keys above are safe to attach unconditionally: Xcode's own
# capability database knows them, they are enabled by ticking a box, and
# automatic signing adds them to the App ID on the next build. A key that is
# granted by REQUEST — or that the toolchain does not recognise at all — is on a
# completely different footing, because attaching one does not degrade. It fails
# the whole signing step, which takes the memory keys down with it, and those
# are what stopped the loading-transition kills. That is why
# `com.apple.developer.networking.multicast` was removed outright once the
# broadcast fan-out in kl_shim.c made it unnecessary.
#
# So a risky key gets a knob rather than a second authored file — two entitlement
# files that have to agree is exactly how they stop agreeing. The authored file
# is the union; the knob FILTERS a generated copy.
#
#   com.apple.developer.low-latency-streaming (KLEPTON_LOW_LATENCY=0 drops it)
#       Added at the user's request while chasing a streaming regression. Note
#       honestly what is and is not known about it: this key does not appear in
#       the XROS SDK or in Xcode's Library tree — but neither do the two memory
#       keys, which certainly do work, so that search is INCONCLUSIVE and not
#       evidence the key is wrong. If the build starts failing with "doesn't
#       include the com.apple.developer.low-latency-streaming entitlement" (a
#       grant you must be given) or with an unknown-entitlement error (a key the
#       platform does not know), set KLEPTON_LOW_LATENCY=0 and the memory keys
#       survive. Do not diagnose a stream problem from its presence alone —
#       runtime/xr/kl_openxr.c's KL_XR_PROFILE is the A/B for the change that
#       actually altered what the client publishes.
#   com.apple.developer.kernel.extended-virtual-addressing (KLEPTON_EVA=0 drops it)
#       A paid-program capability. Xcode's own capability table (DVTPortal.framework,
#       DVTPortalCachedPortalCapabilities.json; read in Xcode 27.1 on 2026-10-11) lists
#       its validTeamTypes as the Developer Program and the Enterprise Program only,
#       while Increased Memory Limit also names XCODE_FREE_PROGRAM, and Low-Latency
#       Streaming the Developer Program alone. So a build for a free Personal Team
#       drops this key and the streaming key and KEEPS the memory limit, which is the
#       one that stopped the jetsam kills. What a guest loses without it is address
#       space above the default ceiling, not resident memory.
OPTIONAL_ENTITLEMENTS = {
    "com.apple.developer.low-latency-streaming": "KLEPTON_LOW_LATENCY",
    "com.apple.developer.kernel.extended-virtual-addressing": "KLEPTON_EVA",
    # The play area's shared rooms (KleptonRoomStore.swift). Without it each
    # game keeps its own.
    "keychain-access-groups": "KLEPTON_SHARED_KEYCHAIN",
}
ENTITLEMENTS_FILE = "Klepton.entitlements"
_dropped = [k for k, env in OPTIONAL_ENTITLEMENTS.items()
            if os.environ.get(env, "1") == "0"]
if ENTITLEMENTS and _dropped:
    import plistlib
    src = pathlib.Path(__file__).with_name("Klepton.entitlements")
    d = plistlib.loads(src.read_bytes())
    for k in _dropped:
        d.pop(k, None)
    out = pathlib.Path(__file__).with_name("build") / "Klepton-filtered.entitlements"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(plistlib.dumps(d))
    ENTITLEMENTS_FILE = "build/Klepton-filtered.entitlements"

ENTITLEMENTS_SETTING = (f"\t\t\t\tCODE_SIGN_ENTITLEMENTS = {ENTITLEMENTS_FILE};\n"
                        if ENTITLEMENTS else "")
GUEST = KLT["libs"].split()
ANGLE = ["ANGLE_libEGL", "ANGLE_libGLESv2"]
# ...and the OTHER renderer, for a guest whose graphics API is Vulkan (BONELAB).
# Same treatment as ANGLE for the same reasons: one driver for every guest that
# asks, dlopened by path at runtime (runtime/gfx/kl_vulkan.c), and 4.6 MB a slice —
# so making it per-target would cost a table row to save less than the row.
#
# OPTIONAL, unlike ANGLE, and that is the only difference: MoltenVK is a
# vendored prebuilt (`make mvk`) that a bare checkout does not have, and three of
# the four targets never ask for Vulkan. Absent, kl_vulkan.c's __has_include stub
# refuses BY NAME, which is the right answer for those three; making it mandatory
# would stop everyone's build for a dependency most targets do not use.
MVK = ["MoltenVK"] if os.path.isdir(
    os.path.join(HERE, "Frameworks", "MoltenVK.xcframework")) else []
# Where each set is staged on the SOURCE side. The guest's is per-target so the
# two apps do not overwrite each other's translations in place; ANGLE's is not,
# because it is the same renderer for every guest. Inside the built .app both
# land flat in Frameworks/, which is why kl_app.c needs to know nothing about it.
GUEST_DIR = f"Frameworks/{KLT['name']}"
ANGLE_DIR = "Frameworks"
# Both sets are embedded-not-linked, so the pbxproj treatment is identical and
# the generator stays data-driven — the two lists differ only in what a missing
# one means, which is why main() reports them separately.
SC2 = ["Scotland2Core", "SC2_libtinyxml2"] if os.environ.get("KLEPTON_SCOTLAND2") == "1" else []
if SC2:
    if os.environ.get('SC2_TEST_SET', 'tinyxml2') in ('paper','hooks','customtypes','lapiz','bsml','songcore','metacore','songdownloader','coremods'):
        SC2.append('SC2_libpaper2_scotland2')
    if os.environ.get('SC2_TEST_SET') in ('hooks','customtypes','lapiz','bsml','songcore','metacore','songdownloader','coremods'):
        SC2 += ['SC2_'+name for name in ['libbeatsaber-hook','libsl2_abi','libcxx_shared','libsc2_flamingo','libsc2_flamingo_probe']]
    if os.environ.get('SC2_TEST_SET') in ('customtypes','lapiz','bsml','songcore','metacore','songdownloader','coremods'):
        SC2 += ['SC2_libcustom-types','SC2_libsc2_customtypes_probe']
    if os.environ.get('SC2_TEST_SET') in ('lapiz','bsml','songcore','metacore','songdownloader','coremods'):
        SC2 += ['SC2_liblapiz']
    if os.environ.get('SC2_TEST_SET') in ('bsml','songcore','metacore','songdownloader','coremods'):
        SC2 += ['SC2_libbsml','SC2_libsc2_bsml_probe']
    if os.environ.get('SC2_TEST_SET') in ('songcore','metacore','songdownloader','coremods'):
        SC2 += ['SC2_libsongcore','SC2_libsc2_songcore_probe']
    if os.environ.get('SC2_TEST_SET') in ('metacore','songdownloader','coremods'):
        SC2 += ['SC2_'+name for name in ['libmetacore','libsc2_metacore_probe','libweb-utils','libbeatsaverplusplus','libzip','libsongdetails']]
    if os.environ.get('SC2_TEST_SET') in ('songdownloader','coremods'):
        SC2 += ['SC2_libplaylistcore','SC2_libsongdownloader']
    if os.environ.get('SC2_TEST_SET') == 'coremods':
        SC2 += ['SC2_libdatakeeper','SC2_libunicode']
    # The bundle id is the target's usual one (or AVP Play's prefix): mods are an update of the same app.
    if KLT['name'] != 'beatsaber140':
        raise SystemExit('Scotland2 mods exist for beatsaber140 only.')
    if os.environ.get('SC2_BUILD_MODE') == 'release':
        import json as _sc2_json
        _sc2_manifest = '../ports/scotland2/port1453/selected-mods.json' if SC2_PROFILE == '1.45.3' else '../ports/scotland2/selected-mods.json'
        with open(os.path.join(HERE, _sc2_manifest)) as _sc2_file:
            _sc2_selection = _sc2_json.load(_sc2_file)
        SC2 = ['Scotland2Core', 'SC2_libcxx_shared'] + [
            'SC2_' + os.path.basename(path)[:-3] for path in _sc2_selection['metadata']]
    for name in SC2:
        if not os.path.isdir(os.path.join(HERE, 'Frameworks', name + '.xcframework')):
            raise SystemExit('Use ports/scotland2/tools/build_release.py to stage the selected release.' if os.environ.get('SC2_BUILD_MODE')=='release' else 'Run bash ports/scotland2/package.sh xros before generating the diagnostic app.')
# The newer APK carries its own Android C++ runtime. The mod composition has
# one explicitly selected shared runtime for the game and all guest mods.
# Both translate to libcxx_shared.framework, so embedding both is ambiguous.
if SC2 and 'SC2_libcxx_shared' in SC2:
    GUEST = [name for name in GUEST if name not in ('libc++_shared', 'libcxx_shared')]
EMBED = GUEST + ANGLE + MVK + SC2


def detect_team():
    if os.environ.get("KLEPTON_TEAM"):
        return os.environ["KLEPTON_TEAM"]
    try:
        pem = subprocess.run(["security", "find-certificate", "-c", "Apple Development",
                              "-p"], capture_output=True, text=True).stdout
        subj = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                              input=pem, capture_output=True, text=True).stdout
        m = re.search(r"OU\s*=\s*([A-Z0-9]{10})", subj)
        if m:
            return m.group(1)
    except Exception:
        pass
    return ""


TEAM = detect_team()

_n = [0]
def oid(tag):
    _n[0] += 1
    return ("KLEPT0N" + f"{_n[0]:04d}" + tag).upper().ljust(24, "0")[:24]


PROJ    = oid("PROJ");  TARGET = oid("TGT");   PRODUCT = oid("PROD")
G_ROOT  = oid("GROOT"); G_SRC  = oid("GSRC");  G_FW    = oid("GFW");  G_PROD = oid("GPRODS")
BP_SRC  = oid("BPSRC"); BP_FRM = oid("BPFRM"); BP_EMB  = oid("BPEMB")
CL_PROJ = oid("CLPRJ"); CL_TGT = oid("CLTGT")
C_PRJ_D = oid("CPRJD"); C_PRJ_R = oid("CPRJR")
C_TGT_D = oid("CTGTD"); C_TGT_R = oid("CTGTR")
F_C = oid("FC"); F_H = oid("FH"); F_BRIDGE = oid("FBRDG")
B_C = oid("BC")
# The app icon: an asset catalog holding one solid-image-stack per target that
# has one (AppIcon-<name>.solidimagestack). Targets without one build exactly
# as before - the catalog compiles empty for them and no APPICON setting is
# emitted, so the icon is strictly additive.
F_ASSETS = oid("FASSET"); B_ASSETS = oid("BASSET"); BP_RES = oid("BPRES")
import os as _os
# The Test switches an installer wants on from the start (KL_TEST_ON below):
# names of environment variables and nothing else, so that what lands in the
# project file and in Info.plist cannot be anything but a list of names.
# KLEPTON_TEST_ON_EXTRA adds a tester's own names to the installer's: AVP Play
# sets KLEPTON_TEST_ON itself and drops one from the shell, so a test copy that
# needs one more switch (e.g. KL_FOVE_SUBPASS_HELPERS) names it here. The
# target's own "test_on" (targets.py) comes last: the switches a game starts
# with for every player. Unset and empty: the same list as before, byte for
# byte.
TEST_ON = ",".join(dict.fromkeys(
    n for n in re.split(r"[,\s]+", os.environ.get("KLEPTON_TEST_ON", "") + ","
                        + os.environ.get("KLEPTON_TEST_ON_EXTRA", "") + ","
                        + KLT.get("test_on", ""))
    if re.fullmatch(r"KL_[A-Z0-9_]{1,60}", n)))

# The OVRPlugin version the game ships (targets.ovrp_guest_version), for the
# profile for untested games; KLOvrpGuestVersion in Info.plist. The reader only
# ever returns digits and dots or "", so nothing else reaches the project.
OVRP_GUEST_VERSION = targets_mod.ovrp_guest_version(KLT["tree"]) if KLT.get("tree") else ""
if not re.fullmatch(r"(1\.\d{1,3}\.\d{1,3})?", OVRP_GUEST_VERSION):
    OVRP_GUEST_VERSION = ""
if OVRP_GUEST_VERSION:
    print(f"  OVRPlugin in the game: {OVRP_GUEST_VERSION}", file=sys.stderr)

# The language a game starts with (KLEPTON_GAME_LANGUAGE: "device", "en" or a
# locale such as "fr_FR"; AVP Play's setting for the games it installs),
# KLGameLanguageDefault in Info.plist (KleptonLanguage). A choice made in the
# game wins over it. Unset, or not of that shape: empty, the target's default.
GAME_LANGUAGE = os.environ.get("KLEPTON_GAME_LANGUAGE", "").strip()
if GAME_LANGUAGE and not re.fullmatch(r"device|en|[a-z]{2,3}_[A-Z]{2}", GAME_LANGUAGE):
    print(f"gen_xcodeproj: KLEPTON_GAME_LANGUAGE={GAME_LANGUAGE!r} is not device, en or a locale "
          f"like fr_FR - the target's default stays", file=sys.stderr)
    GAME_LANGUAGE = ""

# Typed wire values only: never interpolate arbitrary installer input into a project.
CONTROLLER_MODE = os.environ.get("KLEPTON_CONTROLLER_MODE", "standard")
if CONTROLLER_MODE not in ("standard", "surreal"):
    raise ValueError("KLEPTON_CONTROLLER_MODE must be standard or surreal")
# AVP Play's "Surreal Touch support" (KLEPTON_SURREAL_SUPPORT=0: off). Off, the game is
# built for standard controllers only: Standard mode whatever was asked, no SDK fetched
# or linked, and KLSurrealAvailable NO, so the start window offers no controller type and
# no Surreal panel (KleptonControllerMode.available). On, as before.
SURREAL_SUPPORT = os.environ.get("KLEPTON_SURREAL_SUPPORT", "1") != "0"
if not SURREAL_SUPPORT and CONTROLLER_MODE == "surreal":
    print("gen_xcodeproj: Surreal Touch support is off (KLEPTON_SURREAL_SUPPORT=0) - "
          "building for standard controllers", file=sys.stderr)
    CONTROLLER_MODE = "standard"

# Surreal Interactive's visionOS SDK (github.com/surreal-interactive/SwiftSDKBinary,
# Apache-2.0), the pose source of Surreal mode (KleptonSurrealSDK.swift,
# docs/SURREAL.md). A STATIC framework, so linked and never embedded.
#
# Linked into EVERY build, whichever mode it starts in: the mode is the player's
# choice in the game's start window, read when the game starts
# (KleptonControllers.usesSurrealSDK), so a game installed for standard
# controllers can be switched to Surreal Touch there and gets the SDK as well –
# its owner asked for exactly that (2026-10-29). In Standard mode nothing calls
# it. The generator fetches it the first time (tools/surreal_sdk_fetch.sh pins
# and verifies it). A build that STARTS in Surreal mode stops when it cannot have
# it, rather than quietly shipping the old Bluetooth backend; any other build goes
# on without it and says so – a GitHub outage must not stop a game that has
# nothing to do with Surreal Touch (Surreal mode then uses the Bluetooth backend).
# KLEPTON_SURREAL_SDK=0 builds without it on purpose. Only when run as the
# generator, not when a test loads this file.
SVR_NAME = "surreal_interactive_openxr_framework"
_svr_dir = os.path.join(HERE, "Frameworks", f"{SVR_NAME}.xcframework")
_svr_wanted = SURREAL_SUPPORT and os.environ.get("KLEPTON_SURREAL_SDK", "1") != "0"
_svr_fetched = os.path.isdir(_svr_dir) and any(
    n.startswith(".klepton-fetched-") for n in os.listdir(_svr_dir))
if _svr_wanted and not _svr_fetched and __name__ == "__main__":
    # Its progress goes to stderr, like everything else this says: run.sh reads the generator's
    # stdout through `| head -1`, and the fetch's second line then hit a closed pipe – `set -e`
    # ended it right after the download, and every first Surreal build stopped here (AVP Play's
    # installer, 2026-10-29).
    if subprocess.run([os.path.join(HERE, "..", "tools", "surreal_sdk_fetch.sh")],
                      stdout=sys.stderr).returncode != 0:
        if CONTROLLER_MODE == "surreal":
            sys.exit("!! a Surreal Touch build needs Surreal Interactive's SDK, and it could not be "
                     "fetched (above). Check the connection and build again, stage it with "
                     "`make surreal-sdk`, or set KLEPTON_SURREAL_SDK=0 to build with Klepton's "
                     "own Bluetooth backend instead.")
        print("  Surreal Interactive's SDK could not be fetched (above) - building without it; "
              "Surreal Touch in this game then uses Klepton's own Bluetooth backend. The next "
              "build tries again.", file=sys.stderr)
SURREAL_SDK = _svr_wanted and os.path.isfile(
    os.path.join(_svr_dir, "xros-arm64", f"{SVR_NAME}.framework", SVR_NAME))

# The render scale an installer wants the game to START at (KLEPTON_RENDER_SCALE,
# a fraction such as 0.75; AVP Play sets it with its MetalFX switch). It replaces
# the target's own default (targets.py "render_scale") as KLRenderScaleDefault, so
# a choice made in the game still wins. Unset: the target's, as before. Anything
# that is not a number in 0.5..1.0 (the range KleptonTuning's targetRenderScale
# accepts; outside it the bundle would quietly start at 1.0) is said and ignored.
RENDER_SCALE = float(KLT["render_scale"])
_rs_env = os.environ.get("KLEPTON_RENDER_SCALE", "").strip()
if _rs_env:
    try:
        _rs = float(_rs_env)
    except ValueError:
        _rs = None
    if _rs is not None and 0.5 <= _rs <= 1.0:
        RENDER_SCALE = _rs
    else:
        print(f"gen_xcodeproj: KLEPTON_RENDER_SCALE={_rs_env!r} is not a fraction in 0.5..1.0 "
              f"- the target's {RENDER_SCALE:.2f} stays", file=sys.stderr)

# The frame generation a tester's install starts at (KLEPTON_FRAMEGEN=off|ahead|between):
# replaces the target's own (targets.py "framegen") as KLFrameGenDefault, so a choice
# made in the game still wins. Unset: the target's, as before.
FRAMEGEN = KLT["framegen"]
_fg_env = os.environ.get("KLEPTON_FRAMEGEN", "").strip().lower()
if _fg_env:
    if _fg_env in ("off", "ahead", "between"):
        FRAMEGEN = _fg_env
    else:
        print(f"gen_xcodeproj: KLEPTON_FRAMEGEN={_fg_env!r} is not off, ahead or between "
              f"- the target's {FRAMEGEN} stays", file=sys.stderr)

# Compile-time launcher opt-in (also drives LAUNCHER_COND + the display name below).
_launcher_on = _os.environ.get("KL_CUSTOM_LAUNCHER", "").strip().lower() in ("1", "true", "yes", "on")
# NB: the bundle id is deliberately NOT switched by the launcher flag. It is a SIGNING
# identity, and forcing an unregistered id (e.g. com.noosphere.hl1vr) makes every local
# device build fail code-signing and silently relaunch the old app. For the neutral
# store id, register the App ID first, then archive with an explicit
# KLEPTON_BUNDLE_ID=com.noosphere.hl1vr. Local launcher builds keep the dev id and sign.
_assets_dir = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), "Assets.xcassets")
# A target without an icon in the shared catalog but with a Steam app id
# (targets.py "steam_app") gets its icon and start picture from Steam's store
# artwork, built into build/steamart/<target> by steam_art.py. The installer's
# own stacks, when it left any, still win: that directory is only looked at
# second. KL_NO_STEAM_ART=1 skips the fetch (the build is then iconless, as a
# target with no artwork always was).
_steam_art_dir = _os.path.join(HERE, "build", "steamart", KLT['name'])
def _art_path(n):
    for d in (_assets_dir, _steam_art_dir):
        p = _os.path.join(d, n)
        if _os.path.isdir(p):
            return p
    return None
if (KLT['steam_app'] and _os.environ.get("KL_NO_STEAM_ART", "") != "1"
        and not _os.path.isdir(_os.path.join(_assets_dir, f"AppIcon-{KLT['name']}.solidimagestack"))):
    # On stderr: run.sh keeps this script's first stdout line, "wrote ...".
    subprocess.run([sys.executable, _os.path.join(HERE, "steam_art.py"),
                    KLT['steam_app'], KLT['name'], _steam_art_dir], stdout=sys.stderr)
def _stack_exists(n): return _art_path(n + ".solidimagestack") is not None
# With the launcher on, a trademark-free monogram icon (AppIcon-<t>-launcher) replaces
# the target's normal icon when one is present; otherwise the normal icon stands. Both
# are gitignored, so this is strictly local/opt-in.
_icon_base = f"AppIcon-{KLT['name']}"
_icon_name = (f"{_icon_base}-launcher"
              if _launcher_on and _stack_exists(f"{_icon_base}-launcher")
              else _icon_base)
HAS_ICON = _stack_exists(_icon_name)
ASSETCATALOG_SETTING = ((f"\t\t\t\tASSETCATALOG_COMPILER_APPICON_NAME = \"{_icon_name}\";\n"
                         if HAS_ICON else "")
                        # Info.plist's CFBundleIconName is $(KL_APPICON_NAME) —
                        # always defined so expansion never leaves a literal
                        # "$(...)" in the bundle; the placeholder names no stack
                        # and behaves exactly like the old no-icon build.
                        + f"\t\t\t\tKL_APPICON_NAME = \"{_icon_name if HAS_ICON else 'AppIcon'}\";\n"
                        # Same route for the target's hand-matting default
                        # (targets.py "hand_matting"): Info.plist's
                        # KLHandMattingDefault is $(KL_HAND_MATTING_DEFAULT).
                        + f"\t\t\t\tKL_HAND_MATTING_DEFAULT = {1 if KLT['hand_matting'] else 0};\n"
                        # The target's gamepad face-button layout (targets.py
                        # "gamepad_map"), KLGamepadMap in Info.plist. Quoted: the
                        # value carries '=' and ','.
                        + f"\t\t\t\tKL_GAMEPAD_MAP = \"{KLT['gamepad_map']}\";\n"
                        # ...and its render-scale default (targets.py
                        # "render_scale", or KLEPTON_RENDER_SCALE above),
                        # KLRenderScaleDefault in Info.plist.
                        + f"\t\t\t\tKL_RENDER_SCALE_DEFAULT = {RENDER_SCALE:.2f};\n"
                        # ...and whether in-game video is tried by default
                        # (targets.py "video"), KLVideoDefault in Info.plist.
                        + f"\t\t\t\tKL_VIDEO_DEFAULT = {1 if KLT['video'] else 0};\n"
                        # ...whether the guest boots by itself or waits for the
                        # Start button (targets.py "autoboot"), and what the
                        # foveated-rendering setting starts at ("foveation").
                        + f"\t\t\t\tKL_AUTOBOOT_DEFAULT = {1 if KLT['autoboot'] else 0};\n"
                        + f"\t\t\t\tKL_FOVEATION_DEFAULT = {1 if KLT['foveation'] else 0};\n"
                        # ...and the number that drops a stored foveation
                        # choice once (targets.py "foveation_reset"),
                        # KLFoveationReset in Info.plist.
                        + f"\t\t\t\tKL_FOVEATION_RESET = {int(KLT['foveation_reset'])};\n"
                        # ...what edge smoothing for a Vulkan guest starts at
                        # (targets.py "vk_msaa", KLVkMsaaDefault) and how
                        # finely the picture is put on the display ("output",
                        # KLOutputDefault).
                        + f"\t\t\t\tKL_VK_MSAA_DEFAULT = {int(KLT['vk_msaa'])};\n"
                        # ...and the level foveated rendering for a Vulkan
                        # guest starts at (targets.py "vk_foveation",
                        # KLVkFoveationDefault).
                        + f"\t\t\t\tKL_VK_FOVEATION_DEFAULT = {KLT['vk_foveation']};\n"
                        # ...and the number that drops a stored resolution and
                        # foveation level once (targets.py "picture_reset").
                        + f"\t\t\t\tKL_PICTURE_RESET = {int(KLT['picture_reset'])};\n"
                        # ...and the frame rate the start window starts at
                        # (targets.py "max_rate", KLMaxRateDefault): 100 or 120.
                        + f"\t\t\t\tKL_MAX_RATE_DEFAULT = {int(KLT['max_rate'])};\n"
                        # ...and the frame generation it starts with (targets.py
                        # "framegen", KLFrameGenDefault): off, ahead or between.
                        + f"\t\t\t\tKL_FRAMEGEN_DEFAULT = {FRAMEGEN};\n"
                        + f"\t\t\t\tKL_OUTPUT_DEFAULT = {KLT['output']};\n"
                        # ...and which front the app shows: the boot report
                        # this project was developed against, or the start
                        # screen an installed game presents (KleptonApp.swift,
                        # `productStart`). An installer asks for the second with
                        # KLEPTON_START_SCREEN=1; KLStartScreen in Info.plist.
                        + f"\t\t\t\tKL_START_SCREEN = {1 if os.environ.get('KLEPTON_START_SCREEN') == '1' else 0};\n"
                        # ...and the OVRPlugin version the game ships
                        # (OVRP_GUEST_VERSION above; KLOvrpGuestVersion).
                        + f"\t\t\t\tKL_OVRP_GUEST_VERSION = \"{OVRP_GUEST_VERSION}\";\n"
                        # ...and which Test switches this installation has ON
                        # until its owner says otherwise in the game
                        # (KLEPTON_TEST_ON, the variables' names with commas
                        # between them; KLTestOn in Info.plist,
                        # KleptonTestSwitches). An installer names them for
                        # someone who wants a change that is built in but off
                        # in every game at once, without a visit to each.
                        + f"\t\t\t\tKL_TEST_ON = \"{TEST_ON}\";\n"
                        # ...and whether it is a test build (TEST_BUILD above;
                        # KLTestBuild in Info.plist).
                        + f"\t\t\t\tKL_TEST_BUILD = {1 if TEST_BUILD else 0};\n"
                        # ...and whether the play area (KleptonRoom*.swift) is on in
                        # this game until its owner says otherwise there: on unless
                        # the installer says KLEPTON_ROOM_GUARD=0 (AVP Play's
                        # setting for games it installs); KLRoomGuardDefault.
                        + f"\t\t\t\tKL_ROOM_GUARD_DEFAULT = {0 if os.environ.get('KLEPTON_ROOM_GUARD') == '0' else 1};\n"
                        # ...and the language it starts with (GAME_LANGUAGE
                        # above; KLGameLanguageDefault, empty for the target's).
                        + f"\t\t\t\tKL_GAME_LANGUAGE_DEFAULT = \"{GAME_LANGUAGE}\";\n"
                        # ...and what KleptonGameData.swift reads: which files
                        # this build's guest needs and the store ids to fetch
                        # them by. KL_DATA_TREE is empty for a target without the
                        # panel, which is how the app tells.
                        + f"\t\t\t\tKL_DATA_TREE = \"{KLT['tree'] if KLT['data_panel'] else ''}\";\n"
                        + f"\t\t\t\tKL_DATA_APK = \"{KLT['apk']}\";\n"
                        + f"\t\t\t\tKL_DATA_OBB_REL = \"{KLT['obb']}\";\n"
                        + f"\t\t\t\tKL_DATA_OBB_FILE = \"{KLT['obb_file']}\";\n"
                        + f"\t\t\t\tKL_DATA_APK_ID = \"{KLT['meta_apk_id']}\";\n"
                        + f"\t\t\t\tKL_DATA_OBB_ID = \"{KLT['meta_obb_id']}\";\n")

# The privacy strings, as BUILD SETTINGS and from here.
#
# They used to live only in Info.plist, and that is not a safe place for them:
# open the generated project in Xcode, touch the target's Info tab, and Xcode
# MOVES every usage description it recognises out of the plist and into the
# project file as INFOPLIST_KEY_* (it is consolidating them where
# GENERATE_INFOPLIST_FILE reads them). The plist is left without them — and the
# project file is rewritten by this script on the next build. So the strings
# vanish from both, nothing fails, and the app is killed by TCC two seconds
# after launch the moment it asks for accessory tracking (twice now: Beat Saber
# 1.40, then batman). Emitted here, they are in every project this writes,
# wherever else they do or do not survive. visionos/run.sh checks the product.
USAGE_DESCRIPTIONS = {
    "NSMicrophoneUsageDescription": "Klepton uses the microphone only when you turn on the Microphone toggle, so a running app (such as Steam Link voice chat) can hear you.",
    "NSWorldSensingUsageDescription": "Klepton reads the headset's own pose, which is the head pose the guest renders from, the floor and walls of your room for a mixed-reality game, and, when you measure your room, its floor, walls and furniture to find the play area.",
    "NSHandsTrackingUsageDescription": "Klepton maps your hands onto the guest's two Touch controllers.",
    "NSAccessoryTrackingUsageDescription": "Klepton maps PSVR2 Sense controllers onto the guest's two Touch controllers.",
    "NSBluetoothAlwaysUsageDescription": "Klepton connects to spatial controllers that are not visible to the system as standard game controllers.",
    "NSLocalNetworkUsageDescription": "Steam Link finds the PCs on your network by broadcasting to them, and streams from the one you pair with.",
}
_usage_settings = "".join(
    '\t\t\t\tINFOPLIST_KEY_%s = "%s";\n' % (k, v.replace("\\", "\\\\").replace('"', '\\"'))
    for k, v in USAGE_DESCRIPTIONS.items())
ASSETCATALOG_SETTING += _usage_settings
ASSETCATALOG_SETTING += f'\t\t\t\tKL_CONTROLLER_MODE_DEFAULT = "{CONTROLLER_MODE}";\n'
ASSETCATALOG_SETTING += f'\t\t\t\tKL_SURREAL_AVAILABLE = {"YES" if SURREAL_SUPPORT else "NO"};\n'

# Trim the asset catalog to THIS target. The shared Assets.xcassets holds one icon
# stack per target (~26), and actool compiles ALL of them into the app's Assets.car
# (~75 MB of icons the app never uses). So build a per-target catalog under build/
# (gitignored) carrying only this target's normal + launcher stacks, and reference
# THAT below instead of the full catalog. Iconless targets get an empty catalog,
# which builds exactly as the shared one did for them.
import shutil as _shutil
_appicon_catalog = f"build/appicon/{KLT['name']}.xcassets"
_cat_abs = _os.path.join(_os.path.dirname(_os.path.abspath(__file__)), _appicon_catalog)
_shutil.rmtree(_cat_abs, ignore_errors=True)
_os.makedirs(_cat_abs, exist_ok=True)
_src_contents = _os.path.join(_assets_dir, "Contents.json")
if _os.path.exists(_src_contents):
    _shutil.copy(_src_contents, _os.path.join(_cat_abs, "Contents.json"))
else:
    open(_os.path.join(_cat_abs, "Contents.json"), "w").write(
        '{\n  "info" : { "author" : "xcode", "version" : 1 }\n}\n')
for _stack in (_icon_base, f"{_icon_base}-launcher"):
    _s = _art_path(_stack + ".solidimagestack")
    if _s:
        _shutil.copytree(_s, _os.path.join(_cat_abs, _stack + ".solidimagestack"))
# The start screen's picture, if an installer left one for this target
# (StartHero-<target>.imageset) or steam_art.py made one. It goes into the app
# under ONE name, so the Swift side asks for "StartHero" whatever the guest is.
_hero = _art_path(f"StartHero-{KLT['name']}.imageset")
if _hero:
    _shutil.copytree(_hero, _os.path.join(_cat_abs, "StartHero.imageset"))

# The two files apktool decodes out of the APK and a headset cannot (the
# manifest is binary XML in the archive): carried as data assets, so the in-app
# data panel can put them beside the assets it unpacks. They describe exactly
# the APK this build's libraries came from, which is why they may ride along.
if KLT['data_panel']:
    _root = _os.path.dirname(_os.path.dirname(_os.path.abspath(__file__)))
    for _asset, _file in (("KLMeta-apktool", "apktool.yml"),
                          ("KLMeta-manifest", "AndroidManifest.xml")):
        _src = _os.path.join(_root, KLT['tree'], _file)
        if not _os.path.isfile(_src):
            continue
        _ds = _os.path.join(_cat_abs, _asset + ".dataset")
        _os.makedirs(_ds, exist_ok=True)
        _shutil.copy(_src, _os.path.join(_ds, _file))
        open(_os.path.join(_ds, "Contents.json"), "w").write(
            '{\n  "info" : { "author" : "xcode", "version" : 1 },\n'
            '  "data" : [ { "filename" : "%s", "idiom" : "universal" } ]\n}\n' % _file)

# The Swift half of the language split: the App/UI, and the Compositor Services
# renderer adds. A list rather than an id pair each, for the same reason
# the guest libraries are one.
SWIFT = ["KleptonApp.swift", "KleptonCompositor.swift", "KleptonControllers.swift", "KleptonBrinkSelection.swift",
         "KleptonAudio.swift", "KleptonShell.swift", "KleptonTuning.swift",
         "KleptonChroma.swift", "KleptonMic.swift", "KleptonFrameGen.swift",
         "KleptonLauncher.swift", "KleptonHL1.swift", "KleptonGameData.swift",
         "KleptonAlyx.swift", "KleptonText.swift", "KleptonSteamCloud.swift",
         "KleptonBackup.swift", "KleptonMetrics.swift", "KleptonPinch.swift",
         "KleptonTestBuild.swift", "KleptonLogTail.swift", "KleptonAppleMusic.swift", "KleptonModCredits.swift",
         "KleptonArrayViews.swift",
         "KleptonControllerInput.swift", "KleptonControllerMode.swift",
         "KleptonSurrealInput.swift", "KleptonSurrealBackend.swift", "KleptonSurrealView.swift",
         "KleptonSurrealAim.swift", "KleptonSurrealAimView.swift", "KleptonSurrealSDK.swift",
         "KleptonMetalFXPreset.swift", "KleptonMetalFXAlpha.swift",
         "KleptonEyeMax.swift", "KleptonOverlayDepth.swift", "KleptonHologram.swift",
         "KleptonWalkaboutSave.swift",
         "KleptonRealityPresent.swift",
         "KleptonRoomStore.swift", "KleptonRoomSpace.swift", "KleptonBoundary.swift"]
# Steam's connection, sign-in and Steam Cloud (SteamCloudKit/README.md: parts of
# Glovebox's SteamLauncherKit, MIT), compiled into the app's own module. The
# folder is a Swift package only so that its tests run on a Mac.
SWIFT += [f"../SteamCloudKit/Sources/SteamCloudKit/{n}.swift" for n in (
    "CMClient", "Transport", "Messages", "ProtoWire", "KeyValues", "Codec",
    "SteamAuth", "SteamApps", "SteamCloud", "CloudGame")]
swift = [{"name": s, "ref": oid(f"FS{i}"), "bld": oid(f"BS{i}")} for i, s in enumerate(SWIFT)]

swift_buildfiles = "\n".join(
    f'\t\t{s["bld"]} /* {s["name"]} in Sources */ = {{isa = PBXBuildFile; fileRef = {s["ref"]}; }};'
    for s in swift)
swift_filerefs = "\n".join(
    f'\t\t{s["ref"]} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; '
    f'path = {s["name"]}; sourceTree = "<group>"; }};' for s in swift)
swift_children = "\n".join(f'\t\t\t\t{s["ref"]},' for s in swift)
swift_sources  = "\n".join(f'\t\t\t\t{s["bld"]},' for s in swift)
F_RT    = oid("FRT");   B_RT_LNK = oid("BRTLK")
SC2_REF = oid("FSC2META"); SC2_BUILD = oid("BSC2META")
sc2_buildfile = f'{SC2_BUILD} = {{isa = PBXBuildFile; fileRef = {SC2_REF}; }};' if SC2 else ''
sc2_filereference = f'{SC2_REF} = {{isa = PBXFileReference; lastKnownFileType = folder; path = Scotland2Metadata; sourceTree = "<group>"; }};' if SC2 else ''
sc2_resource = f'{SC2_BUILD},' if SC2 else ''
sc2_group = f'{SC2_REF},' if SC2 else ''
# Surreal Interactive's SDK, when SURREAL_SDK: one file ref, one link, no embed.
F_SVR   = oid("FSVR");  B_SVR_LNK = oid("BSVRLK")
SVR_BUILDFILE = (f"\t\t{B_SVR_LNK} /* {SVR_NAME}.xcframework in Frameworks */ = "
                 f"{{isa = PBXBuildFile; fileRef = {F_SVR}; }};\n" if SURREAL_SDK else "")
SVR_FILEREF = (f"\t\t{F_SVR} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; "
               f"name = {SVR_NAME}.xcframework; path = Frameworks/{SVR_NAME}.xcframework; "
               f"sourceTree = \"<group>\"; }};\n" if SURREAL_SDK else "")
SVR_LINK = f"\t\t\t\t{B_SVR_LNK},\n" if SURREAL_SDK else ""
SVR_CHILD = f"\t\t\t\t{F_SVR},\n" if SURREAL_SDK else ""

# One file ref + one embed build-file per embedded framework.
guest = [{"name": g, "ref": oid(f"FG{i}"), "emb": oid(f"BG{i}"),
          "dir": GUEST_DIR if g in GUEST else ANGLE_DIR}
         for i, g in enumerate(EMBED)]

buildfiles = "\n".join(
    f'\t\t{g["emb"]} /* {g["name"]} in Embed */ = {{isa = PBXBuildFile; fileRef = {g["ref"]}; '
    f'settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};'
    for g in guest)

# Quoted, and it is not decoration: a pbxproj value may go unquoted only if it
# is alphanumerics, `_`, `.`, `/` and `-`. `libc++_shared` has a `+` in it, and
# the failure is not a bad reference — the whole project becomes unreadable,
# `xcodebuild` says "damaged ... due to a parse error" and names no file.
filerefs = "\n".join(
    f'\t\t{g["ref"]} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; '
    f'name = "{g["name"]}.xcframework"; path = "{g["dir"]}/{g["name"]}.xcframework"; sourceTree = "<group>"; }};'
    for g in guest)

embeds = "\n".join(f'\t\t\t\t{g["emb"]},' for g in guest)
fwchildren = "\n".join(f'\t\t\t\t{g["ref"]},' for g in guest)

# Compile-time opt-in for the custom "configure, then Start" launcher (hl1/hl2/
# portal). Only when this project is generated with KL_CUSTOM_LAUNCHER set does the
# build carry the KL_CUSTOM_LAUNCHER Swift active-compilation condition that
# klLauncherTitle() (Sources/KleptonLauncher.swift) gates on — so a shipping build
# bakes the launcher in with no runtime env. Default builds (and visionos/run.sh)
# omit it and keep the original autoboot / "Boot"-button shape. Applies to both
# Debug and Release (this is the target-level COMMON block); $(inherited) preserves
# the project-level DEBUG condition.
# With the launcher on, the game builds present a compact trademark-free name; every
# other build (and every other target) keeps its normal display name from targets.py.
# KLT['display'] has already been through targets.display_for (the
# KLEPTON_DISPLAY_OVERRIDE / KLEPTON_DISPLAY_SUFFIX overrides — a second install
# of the same target beside the first needs a name of its own on the headset);
# the launcher's compact name takes the same overrides, so an override still
# wins over it and a suffix is still appended to it.
_launcher_display = {"hl1": "HL1VR", "hl2": "HL2VR", "portal": "P1VR"}
_display = (targets_mod.display_for(_launcher_display[KLT['name']])
            if _launcher_on and KLT['name'] in _launcher_display
            else KLT['display'])
# Inside a quoted pbxproj string: a quote or backslash in an override would end
# the string early and leave the project unparseable.
_display = _display.replace("\\", "\\\\").replace('"', '\\"')
# Trailing newline, glued at line-start to the next setting — the same shape as
# ENTITLEMENTS_SETTING / ASSETCATALOG_SETTING above, so it lands on its own line in
# the output and expands to nothing (no blank line) when the flag is off.
# ...and KL_SURREAL_SDK, which is what lets KleptonSurrealSDK.swift call the SDK at all.
_conds = ((["KL_CUSTOM_LAUNCHER"] if _launcher_on else []) + (["KL_SURREAL_SDK"] if SURREAL_SDK else [])
          + (["KL_SCOTLAND2"] if SC2 else []))
LAUNCHER_COND = (f'\t\t\t\tSWIFT_ACTIVE_COMPILATION_CONDITIONS = "{" ".join(_conds)} $(inherited)";\n'
                 if _conds else "")

# What the installed app says about itself: CFBundleShortVersionString and
# CFBundleVersion. An installer sets these (KLEPTON_APP_VERSION — the guest's own
# version — and KLEPTON_APP_BUILD — which build of it this is), because the app
# list of a headset is the only place a Mac can ask what is installed, and "1.0
# (1)" for every guest answers nothing. Up to three dot-separated integers each,
# which is all the two keys take; anything else keeps the old constant.
import re as _re
def _plist_version(env, dflt):
    v = os.environ.get(env, "")
    return v if _re.fullmatch(r"\d{1,9}(\.\d{1,9}){0,2}", v) else dflt
APP_VERSION = _plist_version("KLEPTON_APP_VERSION", "1.0")
APP_BUILD = _plist_version("KLEPTON_APP_BUILD", "1")

# Build settings for the Beat Saber mods (Info.plist KLScotland2*). Only a build
# with the mods writes them, so every other project file stays as it was.
def _sc2_settings():
    if not SC2:
        return ""
    release = os.environ.get("SC2_BUILD_MODE") == "release"
    test_set = os.environ.get("SC2_TEST_SET", "")
    def on(*sets):
        return 1 if test_set in sets else 0
    rows = {
        "KL_SCOTLAND2_TEST": 1,
        "KL_SCOTLAND2_PROFILE": SC2_PROFILE,
        "KL_SCOTLAND2_DIAGNOSTICS": 0 if release else 1,
        "KL_SCOTLAND2_CUSTOMTYPES": on("customtypes"),
        "KL_SCOTLAND2_METACORE": on("metacore", "songdownloader", "coremods"),
        "KL_SCOTLAND2_SONGDOWNLOADER": on("songdownloader", "coremods"),
        "KL_SCOTLAND2_COREMODS": on("coremods"),
        "KL_SCOTLAND2_SONGCORE": on("songcore", "metacore", "songdownloader", "coremods"),
        "KL_SCOTLAND2_BSML": on("bsml", "songcore", "metacore", "songdownloader", "coremods"),
        "KL_SCOTLAND2_LAPIZ": on("lapiz"),
        "KL_SCOTLAND2_HOOKS": on("hooks", "customtypes", "lapiz", "bsml", "songcore",
                                 "metacore", "songdownloader", "coremods"),
    }
    text = "".join(f'\t\t\t\t{key} = "{value}";\n' for key, value in rows.items())
    # The song search reads the Apple Music history (KleptonAppleMusic.swift); visionOS asks with this text.
    text += ('\t\t\t\tINFOPLIST_KEY_NSAppleMusicUsageDescription = "Beat Saber\'s song search lists the songs you '
             'played recently in Apple Music and finds maps for them on BeatSaver. No song is played from Apple Music.";\n')
    if release:
        text += '\t\t\t\tENABLE_DEBUG_DYLIB = NO;\n'
    return text
SC2_SETTINGS = _sc2_settings()

COMMON = f"""
{SC2_SETTINGS}				CLANG_ENABLE_MODULES = YES;
{ENTITLEMENTS_SETTING}				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = {APP_BUILD};
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = NO;
				GENERATE_INFOPLIST_FILE = YES;
{ASSETCATALOG_SETTING}				INFOPLIST_FILE = Info.plist;
				// One entry per runtime source directory: a runtime header is
				// included by BARE NAME everywhere, including from
				// Klepton-Bridging-Header.h, so each directory holding one has
				// to be on the search path. Same list as the Makefile's
				// RUNTIME_INC; a new runtime/<area>/ needs a line in both.
				//
				// **The source tree comes BEFORE $(inherited)**, which carries
				// $(BUILT_PRODUCTS_DIR)/include — a directory Xcode copies
				// headers into and never removes them from. A header that MOVED
				// between runtime directories therefore leaves a copy at its old
				// bare path there for the life of the derived data, and with
				// $(inherited) first that stale copy shadows the real one: the
				// build fails on a symbol the header plainly declares, naming a
				// Swift line and not a header. Measured after runtime/<area>/
				// existed, on the per-target derived data that predated it.
				HEADER_SEARCH_PATHS = (
					"$(SRCROOT)/../runtime",
					"$(SRCROOT)/../runtime/jni",
					"$(SRCROOT)/../runtime/libc",
					"$(SRCROOT)/../runtime/gfx",
					"$(SRCROOT)/../runtime/xr",
					"$(SRCROOT)/../runtime/media",
					"$(SRCROOT)/../runtime/guest",
					"$(SRCROOT)/../runtime/diag",
					"$(inherited)",
				);
				// Two levels of escaping, and the first attempt had one. The
				// pbxproj layer eats a backslash, and the build system then
				// unquotes the setting VALUE the way a shell would — so a
				// single \" here reaches clang as -DKL_TARGET_DEFAULT=beatsaber
				// and fails with "use of undeclared identifier 'beatsaber'",
				// which names the target and says nothing about quoting.
				GCC_PREPROCESSOR_DEFINITIONS = (
					"$(inherited)",
					"KL_TARGET_DEFAULT=\\\\\\"{KLT['name']}\\\\\\"",
				);
				INFOPLIST_KEY_CFBundleDisplayName = "{_display}";
				INFOPLIST_KEY_GCSupportsControllerUserInteraction = YES;
				// NOT UIApplicationSceneManifest_Generation. Setting it makes Xcode
				// GENERATE the scene manifest and overwrite the one in Info.plist —
				// with UISceneConfigurations as an EMPTY dict, i.e. the window role
				// and no immersive-space role at all. The app then has no immersive
				// scene session, and the failure is invisible from inside: the space
				// "opens", a LayerRenderer runs, drawables arrive fully formed and
				// every frame presents, into nothing. Apple's own template does not
				// set this and declares the manifest by hand; so do we now
				// (visionos/Info.plist). This was the black immersive space.
				LD_RUNPATH_SEARCH_PATHS = (
					"$(inherited)",
					"@executable_path/Frameworks",
				);
				MARKETING_VERSION = {APP_VERSION};
				// VideoToolbox/CoreMedia/CoreVideo are kl_vtdec's, and
				// they were missing here for the whole of the Steam Link arc:
				// kl_vtdec joined RUNTIME_SHIP, `make xros` kept passing because
				// its own link line has them, and nothing built the APP until a
				// second target needed one. The failure is a link error naming
				// VTDecompressionSession*, which reads as a missing SDK rather
				// than as a build setting that was never updated.
				OTHER_LDFLAGS = "-lz -lc++ -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework IOSurface{" -framework Accelerate" if SURREAL_SDK else ""}{" -framework CoreText -framework CoreGraphics" if SC2 else ""}";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				SWIFT_OBJC_BRIDGING_HEADER = "Sources/Klepton-Bridging-Header.h";
{LAUNCHER_COND}				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 7;
				// visionOS 26, not 2.0. The device runs 27 and the SDK is 26, and an
				// app declaring a 2.0 minimum is asking the system for five-major-
				// versions-ago behaviour — which for Compositor Services is not a
				// detail: 26 reworked the drawable model (queryDrawables,
				// Drawable.Target, CompositorContent, Metal 4 residency), and the
				// pre-26 path is what a 2.0 app gets. It also makes those APIs
				// unavailable at compile time, which is how it was noticed at all.
				XROS_DEPLOYMENT_TARGET = 26.0;
"""

PBX = f"""// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{}};
	objectVersion = 60;
	objects = {{

/* Begin PBXBuildFile section */
{sc2_buildfile}
{swift_buildfiles}
		{B_C} /* kl_app.c in Sources */ = {{isa = PBXBuildFile; fileRef = {F_C}; }};
		{B_ASSETS} /* Assets.xcassets in Resources */ = {{isa = PBXBuildFile; fileRef = {F_ASSETS}; }};
		{B_RT_LNK} /* Klepton.xcframework in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_RT}; }};
{SVR_BUILDFILE}{buildfiles}
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
{sc2_filereference}
		{PRODUCT} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};
{swift_filerefs}
		{F_C} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = kl_app.c; sourceTree = "<group>"; }};
		{F_H} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = kl_app.h; sourceTree = "<group>"; }};
		{F_BRIDGE} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "Klepton-Bridging-Header.h"; sourceTree = "<group>"; }};
		{F_ASSETS} = {{isa = PBXFileReference; lastKnownFileType = folder.assetcatalog; path = {_appicon_catalog}; sourceTree = "<group>"; }};
		{F_RT} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = Klepton.xcframework; path = ../build/Klepton.xcframework; sourceTree = "<group>"; }};
{SVR_FILEREF}{filerefs}
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{BP_FRM} = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_RT_LNK},
{SVR_LINK}			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXCopyFilesBuildPhase section */
		{BP_EMB} /* Embed Frameworks */ = {{
			isa = PBXCopyFilesBuildPhase;
			buildActionMask = 2147483647;
			dstPath = "";
			dstSubfolderSpec = 10;
			files = (
{embeds}
			);
			name = "Embed Frameworks";
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXCopyFilesBuildPhase section */

/* Begin PBXGroup section */
		{G_ROOT} = {{
			isa = PBXGroup;
			children = (
				{G_SRC},
				{G_FW},
				{G_PROD},
				{F_ASSETS},
                {sc2_group}
			);
			sourceTree = "<group>";
		}};
		{G_SRC} /* Sources */ = {{
			isa = PBXGroup;
			children = (
{swift_children}
				{F_C},
				{F_H},
				{F_BRIDGE},
			);
			path = Sources;
			sourceTree = "<group>";
		}};
		{G_FW} /* Frameworks */ = {{
			isa = PBXGroup;
			children = (
				{F_RT},
{SVR_CHILD}{fwchildren}
			);
			name = Frameworks;
			sourceTree = "<group>";
		}};
		{G_PROD} /* Products */ = {{
			isa = PBXGroup;
			children = (
				{PRODUCT},
			);
			name = Products;
			sourceTree = "<group>";
		}};
/* End PBXGroup section */

/* Begin PBXNativeTarget section */
		{TARGET} /* {NAME} */ = {{
			isa = PBXNativeTarget;
			buildConfigurationList = {CL_TGT};
			buildPhases = (
				{BP_SRC},
				{BP_FRM},
				{BP_RES},
				{BP_EMB},
			);
			buildRules = ();
			dependencies = ();
			name = {NAME};
			productName = {NAME};
			productReference = {PRODUCT};
			productType = "com.apple.product-type.application";
		}};
/* End PBXNativeTarget section */

/* Begin PBXProject section */
		{PROJ} = {{
			isa = PBXProject;
			attributes = {{
				BuildIndependentTargetsInParallel = 1;
				LastSwiftUpdateCheck = 1600;
				LastUpgradeCheck = 1600;
				TargetAttributes = {{
					{TARGET} = {{ CreatedOnToolsVersion = 16.0; }};
				}};
			}};
			buildConfigurationList = {CL_PROJ};
			compatibilityVersion = "Xcode 14.0";
			developmentRegion = en;
			hasScannedForEncodings = 0;
			knownRegions = (en, Base, );
			mainGroup = {G_ROOT};
			productRefGroup = {G_PROD};
			projectDirPath = "";
			projectRoot = "";
			targets = (
				{TARGET},
			);
		}};
/* End PBXProject section */

/* Begin PBXResourcesBuildPhase section */
		{BP_RES} = {{
			isa = PBXResourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_ASSETS},
                {sc2_resource}
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXResourcesBuildPhase section */

/* Begin PBXSourcesBuildPhase section */
		{BP_SRC} = {{
			isa = PBXSourcesBuildPhase;
			buildActionMask = 2147483647;
			files = (
{swift_sources}
				{B_C},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXSourcesBuildPhase section */

/* Begin XCBuildConfiguration section */
		{C_PRJ_D} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = dwarf;
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				GCC_OPTIMIZATION_LEVEL = 0;
				GCC_PREPROCESSOR_DEFINITIONS = ( "DEBUG=1", "$(inherited)", );
				ONLY_ACTIVE_ARCH = YES;
				SWIFT_ACTIVE_COMPILATION_CONDITIONS = "DEBUG $(inherited)";
				SWIFT_OPTIMIZATION_LEVEL = "-Onone";
			}};
			name = Debug;
		}};
		{C_PRJ_R} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{
				ALWAYS_SEARCH_USER_PATHS = NO;
				CLANG_ENABLE_OBJC_ARC = YES;
				COPY_PHASE_STRIP = NO;
				DEBUG_INFORMATION_FORMAT = "dwarf-with-dsym";
				ENABLE_STRICT_OBJC_MSGSEND = YES;
				SWIFT_COMPILATION_MODE = wholemodule;
			}};
			name = Release;
		}};
		{C_TGT_D} /* Debug */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Debug;
		}};
		{C_TGT_R} /* Release */ = {{
			isa = XCBuildConfiguration;
			buildSettings = {{{COMMON}			}};
			name = Release;
		}};
/* End XCBuildConfiguration section */

/* Begin XCConfigurationList section */
		{CL_PROJ} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{C_PRJ_D},
				{C_PRJ_R},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
		{CL_TGT} = {{
			isa = XCConfigurationList;
			buildConfigurations = (
				{C_TGT_D},
				{C_TGT_R},
			);
			defaultConfigurationIsVisible = 0;
			defaultConfigurationName = Release;
		}};
/* End XCConfigurationList section */
	}};
	rootObject = {PROJ};
}}
"""


def main():
    for g, d, how in [(g, GUEST_DIR, "visionos/mkguest.sh") for g in GUEST] + \
                     [(a, ANGLE_DIR, "visionos/mkangle.sh") for a in ANGLE]:
        p = os.path.join(HERE, d, f"{g}.xcframework")
        if not os.path.isdir(p):
            print(f"!! missing {p} — run {how} first", file=sys.stderr)
            return 1
    if not os.path.isdir(os.path.join(HERE, "..", "build", "Klepton.xcframework")):
        print("!! missing build/Klepton.xcframework — run `make xros` first", file=sys.stderr)
        return 1

    proj = os.path.join(HERE, f"{NAME}.xcodeproj")
    os.makedirs(proj, exist_ok=True)
    with open(os.path.join(proj, "project.pbxproj"), "w") as f:
        f.write(PBX)
    print(f"wrote {proj}")
    print(f"  KLEPTON_TARGET            = {KLT['name']}")
    print(f"  DEVELOPMENT_TEAM = {TEAM or '(NOT DETECTED - set KLEPTON_TEAM)'}")
    # ENTITLEMENTS_FILE, not the literal name: this used to print
    # "Klepton.entitlements" unconditionally, so a build that was actually
    # signing against a FILTERED copy said it was signing against the authored
    # one. A summary that contradicts what went into the pbxproj is worse than
    # no summary — it is the line you would check to rule the filter out.
    print(f"  CODE_SIGN_ENTITLEMENTS = "
          f"{ENTITLEMENTS_FILE if ENTITLEMENTS else '(none - KLEPTON_ENTITLEMENTS=0)'}"
          + (f"  (dropped: {', '.join(_dropped)})" if ENTITLEMENTS and _dropped else ""))
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")
    print(f"  embedded guest frameworks: {', '.join(GUEST)}")
    print(f"  embedded ANGLE:            {', '.join(ANGLE)}")
    # Named either way. An absent MoltenVK is legitimate for three of the four
    # targets and fatal for the fourth, and the difference is invisible until a
    # Vulkan guest is on a headset — so the build says which one it made.
    print(f"  embedded MoltenVK:         "
          + (', '.join(MVK) if MVK else "(none - run 'make mvk' for a Vulkan guest)"))
    if not SURREAL_SUPPORT:
        print("  Surreal Touch:             off (KLEPTON_SURREAL_SUPPORT=0) - standard controllers only")
    else:
        print(f"  Surreal Touch poses:       "
              + ("Surreal Interactive's SDK (linked)" if SURREAL_SDK
                 else "KleptonSurrealBackend (no SDK in this build)")
              + f", starting in {CONTROLLER_MODE} mode")
    return 0


if __name__ == "__main__":
    sys.exit(main())
