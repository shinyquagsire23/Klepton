import Foundation

// Test builds: what only a tester is to see or switch. Nothing asks it at the
// moment — its first user, the live XR head prediction switch on the metrics
// panel, made way for the MetalFX tap (KleptonMetalFX.tapToggle), which every
// build has — but the rule stays, for the next control of that kind. A file
// of its own, with nothing but Foundation, so that the rule can be compiled and
// checked on the Mac (tests/t_testbuild.swift, `make testbuild`).

/// Whether this is a test build — a copy installed beside the real game to try
/// something on the headset — rather than what a player has. What only a tester
/// is to see or switch asks this, and in a player's build it is not there at all.
///
/// Baked in at build time (KLTestBuild in Info.plist, from gen_xcodeproj.py):
/// a test copy is the one with a name of its own (KLEPTON_DISPLAY_SUFFIX or
/// _OVERRIDE, which only the command line sets), a bundle id ending in "-test"
/// or ".test" in any case (regex [-.]test$), KL_TEST_BUILD among
/// KLEPTON_TEST_ON, or KLEPTON_TEST_BUILD=1. Not merely a bundle id of its own
/// or any KLEPTON_TEST_ON: AVP Play sets the first for every install and the
/// second for a player who chose the experimental switches. Or on the launch
/// line, for a run from Xcode or devicectl: KL_TEST_BUILD=1 exactly, and
/// nothing else — KLEPTON_TEST_ON is a build variable of AVP Play's, and what a
/// player's build carries is not the launch line's to judge.
enum KleptonTestBuild {
    /// The launch line's half, apart so that it can be checked on its own
    /// (tests/t_testbuild.swift): only the value "1" counts.
    static func launched(_ env: [String: String]) -> Bool { env["KL_TEST_BUILD"] == "1" }

    static let on: Bool = {
        let baked = (Bundle.main.object(forInfoDictionaryKey: "KLTestBuild") as? String) == "1"
        let atLaunch = launched(ProcessInfo.processInfo.environment)
        if baked || atLaunch {
            NSLog("[app] test build (%@): tester-only controls are on",
                  baked ? "KLTestBuild in Info.plist" : "KL_TEST_BUILD=1 at launch")
        }
        return baked || atLaunch
    }()
}
