// swift-tools-version:5.9
import PackageDescription

// The sources are compiled straight into the game app (visionos/gen_xcodeproj.py lists them). This package is
// only how their tests run on a Mac: `swift test` in this folder.
let package = Package(
    name: "SteamCloudKit",
    platforms: [.visionOS(.v1), .macOS(.v14)],
    targets: [
        .target(name: "SteamCloudKit", linkerSettings: [.linkedLibrary("z")]),
        .testTarget(name: "SteamCloudKitTests", dependencies: ["SteamCloudKit"]),
    ]
)
