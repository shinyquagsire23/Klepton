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
  Sources/              the Swift app layer and kl_app.c (PLANNING §12.6).
"""
import os, subprocess, sys, re

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "Klepton"
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", "dev.klepton.app")
GUEST = ["libmain", "lib_burst_generated", "libunityopus", "libunity", "libil2cpp"]


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
F_SWIFT = oid("FSWFT"); F_C = oid("FC"); F_H = oid("FH"); F_BRIDGE = oid("FBRDG")
B_SWIFT = oid("BSWFT"); B_C = oid("BC")
F_RT    = oid("FRT");   B_RT_LNK = oid("BRTLK")

# One file ref + one embed build-file per guest library.
guest = [{"name": g, "ref": oid(f"FG{i}"), "emb": oid(f"BG{i}")} for i, g in enumerate(GUEST)]

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
				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = NO;
				GENERATE_INFOPLIST_FILE = YES;
				HEADER_SEARCH_PATHS = (
					"$(inherited)",
					"$(SRCROOT)/../runtime",
				);
				INFOPLIST_KEY_CFBundleDisplayName = Klepton;
				INFOPLIST_KEY_UIApplicationSceneManifest_Generation = YES;
				LD_RUNPATH_SEARCH_PATHS = (
					"$(inherited)",
					"@executable_path/Frameworks",
				);
				MARKETING_VERSION = 1.0;
				OTHER_LDFLAGS = "-lz";
				PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID};
				PRODUCT_NAME = "$(TARGET_NAME)";
				SDKROOT = xros;
				SUPPORTED_PLATFORMS = "xros xrsimulator";
				SWIFT_OBJC_BRIDGING_HEADER = "Sources/{NAME}-Bridging-Header.h";
				SWIFT_VERSION = 5.0;
				TARGETED_DEVICE_FAMILY = 7;
				XROS_DEPLOYMENT_TARGET = 2.0;
"""

PBX = f"""// !$*UTF8*$!
{{
	archiveVersion = 1;
	classes = {{}};
	objectVersion = 60;
	objects = {{

/* Begin PBXBuildFile section */
		{B_SWIFT} /* {NAME}App.swift in Sources */ = {{isa = PBXBuildFile; fileRef = {F_SWIFT}; }};
		{B_C} /* kl_app.c in Sources */ = {{isa = PBXBuildFile; fileRef = {F_C}; }};
		{B_RT_LNK} /* Klepton.xcframework in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_RT}; }};
{buildfiles}
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		{PRODUCT} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};
		{F_SWIFT} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = {NAME}App.swift; sourceTree = "<group>"; }};
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
				{F_SWIFT},
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
				{B_SWIFT},
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
    for g in GUEST:
        p = os.path.join(HERE, "Frameworks", f"{g}.xcframework")
        if not os.path.isdir(p):
            print(f"!! missing {p} — run visionos/mkguest.sh first", file=sys.stderr)
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
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")
    print(f"  embedded guest frameworks: {', '.join(GUEST)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
