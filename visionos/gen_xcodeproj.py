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
  Sources/              the Swift app layer and kl_app.c (PLANNING §12.6).
"""
import os, pathlib, subprocess, sys, re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import targets as targets_mod

HERE = os.path.dirname(os.path.abspath(__file__))
KLT = targets_mod.resolve(os.environ.get("KLEPTON_TARGET") or targets_mod.DEFAULT)
# The PRODUCT name, which is also the .xcodeproj's, the .app's and the scheme's.
# The Swift sources are still Klepton*.swift whatever the product is called —
# they are the app layer, shared by every target, and renaming them per build
# would be five file copies to say one thing.
NAME = KLT["product"]
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", KLT["bundle"])

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
#          "dev.klepton.app" cannot be registered to your development team
#
# That pair of errors means the App ID is not set up, not that the entitlements
# are wrong. Enable both capabilities on it (Xcode > Settings > Accounts, or the
# developer portal) — or build with KLEPTON_ENTITLEMENTS=0 to get moving without
# them, at the cost of the memory headroom.
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
#       runtime/kl_openxr.c's KL_XR_PROFILE is the A/B for the change that
#       actually altered what the client publishes.
OPTIONAL_ENTITLEMENTS = {
    "com.apple.developer.low-latency-streaming": "KLEPTON_LOW_LATENCY",
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
# Where each set is staged on the SOURCE side. The guest's is per-target so the
# two apps do not overwrite each other's translations in place; ANGLE's is not,
# because it is the same renderer for every guest. Inside the built .app both
# land flat in Frameworks/, which is why kl_app.c needs to know nothing about it.
GUEST_DIR = f"Frameworks/{KLT['name']}"
ANGLE_DIR = "Frameworks"
# Both sets are embedded-not-linked, so the pbxproj treatment is identical and
# the generator stays data-driven — the two lists differ only in what a missing
# one means, which is why main() reports them separately.
EMBED = GUEST + ANGLE


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

# The Swift half of §12.6's split: the App/UI, and the Compositor Services
# renderer P5b adds. A list rather than an id pair each, for the same reason
# the guest libraries are one.
SWIFT = ["KleptonApp.swift", "KleptonCompositor.swift", "KleptonControllers.swift",
         "KleptonTemplate.swift", "KleptonAudio.swift", "KleptonShell.swift"]
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

COMMON = f"""
				CLANG_ENABLE_MODULES = YES;
{ENTITLEMENTS_SETTING}				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = NO;
				GENERATE_INFOPLIST_FILE = YES;
				INFOPLIST_FILE = Info.plist;
				HEADER_SEARCH_PATHS = (
					"$(inherited)",
					"$(SRCROOT)/../runtime",
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
				INFOPLIST_KEY_CFBundleDisplayName = "{KLT['display']}";
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
				MARKETING_VERSION = 1.0;
				// VideoToolbox/CoreMedia/CoreVideo are kl_vtdec's (SL-10), and
				// they were missing here for the whole of the Steam Link arc:
				// kl_vtdec joined RUNTIME_SHIP, `make xros` kept passing because
				// its own link line has them, and nothing built the APP until a
				// second target needed one. The failure is a link error naming
				// VTDecompressionSession*, which reads as a missing SDK rather
				// than as a build setting that was never updated.
				OTHER_LDFLAGS = "-lz -framework AudioToolbox -framework VideoToolbox -framework CoreMedia -framework CoreVideo";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				SWIFT_OBJC_BRIDGING_HEADER = "Sources/Klepton-Bridging-Header.h";
				SWIFT_VERSION = 5.0;
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
{swift_buildfiles}
		{B_C} /* kl_app.c in Sources */ = {{isa = PBXBuildFile; fileRef = {F_C}; }};
		{B_RT_LNK} /* Klepton.xcframework in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_RT}; }};
{buildfiles}
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		{PRODUCT} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};
{swift_filerefs}
		{F_C} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = kl_app.c; sourceTree = "<group>"; }};
		{F_H} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = kl_app.h; sourceTree = "<group>"; }};
		{F_BRIDGE} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "Klepton-Bridging-Header.h"; sourceTree = "<group>"; }};
		{F_RT} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = Klepton.xcframework; path = ../build/Klepton.xcframework; sourceTree = "<group>"; }};
{filerefs}
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{BP_FRM} = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_RT_LNK},
			);
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
{fwchildren}
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
    print(f"  CODE_SIGN_ENTITLEMENTS = "
          f"{'Klepton.entitlements' if ENTITLEMENTS else '(none - KLEPTON_ENTITLEMENTS=0)'}")
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")
    print(f"  embedded guest frameworks: {', '.join(GUEST)}")
    print(f"  embedded ANGLE:            {', '.join(ANGLE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
