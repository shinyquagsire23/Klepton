#!/usr/bin/env python3
"""Generate KleptonProbe.xcodeproj (visionOS app) with no Xcode GUI needed."""
import os, subprocess, sys, re

HERE = os.path.dirname(os.path.abspath(__file__))
NAME = "KleptonProbe"
BUNDLE_ID = os.environ.get("KLEPTON_BUNDLE_ID", "dev.klepton.probe")

def detect_team():
    if os.environ.get("KLEPTON_TEAM"):
        return os.environ["KLEPTON_TEAM"]
    try:
        pem = subprocess.run(["security", "find-certificate", "-c", "Apple Development",
                              "-p"], capture_output=True, text=True).stdout
        subj = subprocess.run(["openssl", "x509", "-noout", "-subject"],
                              input=pem, capture_output=True, text=True).stdout
        m = re.search(r"OU\s*=\s*([A-Z0-9]{10})", subj)
        if m: return m.group(1)
    except Exception:
        pass
    return ""

TEAM = detect_team()

_n = [0]
def oid(tag):
    _n[0] += 1
    return ("KLEPT0N" + f"{_n[0]:04d}" + tag).upper().ljust(24, "0")[:24]

# --- object ids ---
PROJ      = oid("PROJ");  TARGET   = oid("TGT");   PRODUCT  = oid("PROD")
G_ROOT    = oid("GROOT"); G_SRC    = oid("GSRC");  G_FW     = oid("GFW");  G_PROD = oid("GPRODS")
BP_SRC    = oid("BPSRC"); BP_FRAME = oid("BPFRM"); BP_EMBED = oid("BPEMB")
CL_PROJ   = oid("CLPRJ"); CL_TGT   = oid("CLTGT")
C_PROJ_D  = oid("CPRJD"); C_PROJ_R = oid("CPRJR")
C_TGT_D   = oid("CTGTD"); C_TGT_R  = oid("CTGTR")
F_SWIFT   = oid("FSWFT"); F_C      = oid("FC");    F_H     = oid("FH");   F_BRIDGE = oid("FBRDG")
F_FWA     = oid("FFWA");  F_FWB    = oid("FFWB"); F_FWG = oid("FFWG")
B_SWIFT   = oid("BSWFT"); B_C      = oid("BC")
B_LNKA    = oid("BLNKA"); B_LNKB   = oid("BLNKB")
B_EMBA    = oid("BEMBA"); B_EMBB   = oid("BEMBB"); B_EMBG = oid("BEMBG")

COMMON = f"""
				CLANG_ENABLE_MODULES = YES;
				CODE_SIGN_STYLE = Automatic;
				CURRENT_PROJECT_VERSION = 1;
				DEVELOPMENT_TEAM = {TEAM};
				ENABLE_PREVIEWS = NO;
				GENERATE_INFOPLIST_FILE = YES;
				INFOPLIST_KEY_CFBundleDisplayName = "Klepton Probe";
				INFOPLIST_KEY_UIApplicationSceneManifest_Generation = YES;
				LD_RUNPATH_SEARCH_PATHS = (
					"$(inherited)",
					"@executable_path/Frameworks",
				);
				MARKETING_VERSION = 1.0;
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
		{B_SWIFT} /* app.swift in Sources */ = {{isa = PBXBuildFile; fileRef = {F_SWIFT}; }};
		{B_C} /* probes.c in Sources */ = {{isa = PBXBuildFile; fileRef = {F_C}; }};
		{B_LNKA} /* A in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_FWA}; }};
		{B_LNKB} /* B in Frameworks */ = {{isa = PBXBuildFile; fileRef = {F_FWB}; }};
		{B_EMBA} /* A in Embed */ = {{isa = PBXBuildFile; fileRef = {F_FWA}; settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};
		{B_EMBB} /* B in Embed */ = {{isa = PBXBuildFile; fileRef = {F_FWB}; settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};
		{B_EMBG} /* Guest in Embed */ = {{isa = PBXBuildFile; fileRef = {F_FWG}; settings = {{ATTRIBUTES = (CodeSignOnCopy, RemoveHeadersOnCopy, ); }}; }};
/* End PBXBuildFile section */

/* Begin PBXFileReference section */
		{PRODUCT} /* {NAME}.app */ = {{isa = PBXFileReference; explicitFileType = wrapper.application; includeInIndex = 0; path = {NAME}.app; sourceTree = BUILT_PRODUCTS_DIR; }};
		{F_SWIFT} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.swift; path = {NAME}App.swift; sourceTree = "<group>"; }};
		{F_C} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.c; path = probes.c; sourceTree = "<group>"; }};
		{F_H} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = probes.h; sourceTree = "<group>"; }};
		{F_BRIDGE} = {{isa = PBXFileReference; lastKnownFileType = sourcecode.c.h; path = "{NAME}-Bridging-Header.h"; sourceTree = "<group>"; }};
		{F_FWA} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = KleptonProbeA.xcframework; path = Frameworks/KleptonProbeA.xcframework; sourceTree = "<group>"; }};
		{F_FWB} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = KleptonProbeB.xcframework; path = Frameworks/KleptonProbeB.xcframework; sourceTree = "<group>"; }};
		{F_FWG} = {{isa = PBXFileReference; lastKnownFileType = wrapper.xcframework; name = KleptonGuest.xcframework; path = Frameworks/KleptonGuest.xcframework; sourceTree = "<group>"; }};
/* End PBXFileReference section */

/* Begin PBXFrameworksBuildPhase section */
		{BP_FRAME} = {{
			isa = PBXFrameworksBuildPhase;
			buildActionMask = 2147483647;
			files = (
				{B_LNKA},
				{B_LNKB},
			);
			runOnlyForDeploymentPostprocessing = 0;
		}};
/* End PBXFrameworksBuildPhase section */

/* Begin PBXCopyFilesBuildPhase section */
		{BP_EMBED} /* Embed Frameworks */ = {{
			isa = PBXCopyFilesBuildPhase;
			buildActionMask = 2147483647;
			dstPath = "";
			dstSubfolderSpec = 10;
			files = (
				{B_EMBA},
				{B_EMBB},
				{B_EMBG},
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
				{F_FWA},
				{F_FWB},
				{F_FWG},
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
				{BP_FRAME},
				{BP_EMBED},
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
		{C_PROJ_D} /* Debug */ = {{
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
		{C_PROJ_R} /* Release */ = {{
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
				{C_PROJ_D},
				{C_PROJ_R},
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
    proj = os.path.join(HERE, f"{NAME}.xcodeproj")
    os.makedirs(proj, exist_ok=True)
    with open(os.path.join(proj, "project.pbxproj"), "w") as f:
        f.write(PBX)
    print(f"wrote {proj}")
    print(f"  DEVELOPMENT_TEAM = {TEAM or '(NOT DETECTED - set KLEPTON_TEAM or pick in Xcode)'}")
    print(f"  PRODUCT_BUNDLE_IDENTIFIER = {BUNDLE_ID}")

if __name__ == "__main__":
    main()
