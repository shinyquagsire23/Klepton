#!/usr/bin/env python3
"""Generate Klepton.xcodeproj (the visionOS host app) with no Xcode GUI needed.

Grown from spikes/device-probe/gen_xcodeproj.py, but data-driven: the guest
libraries are a list, not seven hand-written object ids each. That matters
because the set changes — a second target (Steam Link, PLANNING §11) has
different libraries and should not need the generator edited.

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
import os, subprocess, sys, re

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "Klepton"
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", "dev.klepton.app")

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
ENTITLEMENTS_SETTING = ("\t\t\t\tCODE_SIGN_ENTITLEMENTS = Klepton.entitlements;\n"
                        if ENTITLEMENTS else "")
GUEST = ["libmain", "lib_burst_generated", "libunityopus", "libunity", "libil2cpp"]
ANGLE = ["ANGLE_libEGL", "ANGLE_libGLESv2"]
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
SWIFT = [f"{NAME}App.swift", f"{NAME}Compositor.swift", f"{NAME}Controllers.swift",
         f"{NAME}Template.swift", f"{NAME}Audio.swift"]
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
guest = [{"name": g, "ref": oid(f"FG{i}"), "emb": oid(f"BG{i}")} for i, g in enumerate(EMBED)]

buildfiles = "\n".join(
    f'\t\t{g["emb"]} /* {g["name"]} in Embed */ = {{isa = PBXBuildFile; fileRef = {g["ref"]}; '
    f'settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};'
    for g in guest)

filerefs = "\n".join(
    f'\t\t{g["ref"]} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; '
    f'name = {g["name"]}.xcframework; path = Frameworks/{g["name"]}.xcframework; sourceTree = "<group>"; }};'
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
				INFOPLIST_KEY_CFBundleDisplayName = Klepton;
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
				OTHER_LDFLAGS = "-lz -framework AudioToolbox";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				SWIFT_OBJC_BRIDGING_HEADER = "Sources/{NAME}-Bridging-Header.h";
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
		{F_BRIDGE} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "{NAME}-Bridging-Header.h"; sourceTree = "<group>"; }};
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
    for g, how in [(g, "visionos/mkguest.sh") for g in GUEST] + \
                  [(a, "visionos/mkangle.sh") for a in ANGLE]:
        p = os.path.join(HERE, "Frameworks", f"{g}.xcframework")
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
    print(f"  DEVELOPMENT_TEAM = {TEAM or '(NOT DETECTED - set KLEPTON_TEAM)'}")
    print(f"  CODE_SIGN_ENTITLEMENTS = "
          f"{'Klepton.entitlements' if ENTITLEMENTS else '(none - KLEPTON_ENTITLEMENTS=0)'}")
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")
    print(f"  embedded guest frameworks: {', '.join(GUEST)}")
    print(f"  embedded ANGLE:            {', '.join(ANGLE)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
