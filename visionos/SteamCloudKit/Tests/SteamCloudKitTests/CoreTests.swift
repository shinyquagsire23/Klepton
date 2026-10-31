// From Glovebox's SteamLauncherKit tests (MIT, see Sources/SteamCloudKit/README.md), without the tests of the
// parts that were left out (depot decryption, chunk decoders, password sign-in).
import XCTest
@testable import SteamCloudKit

final class CoreTests: XCTestCase {
    func testProtoRoundTrip() throws {
        var h = CMsgProtoBufHeader()
        h.steamID = 76561197960287930
        h.clientSessionID = -5
        h.jobIDSource = 42
        h.targetJobName = "Authentication.BeginAuthSessionViaQR#1"
        let back = try CMsgProtoBufHeader(proto: h.encoded())
        XCTAssertEqual(back.steamID, h.steamID)
        XCTAssertEqual(back.clientSessionID, -5)
        XCTAssertEqual(back.jobIDSource, 42)
        XCTAssertEqual(back.targetJobName, h.targetJobName)
    }


    /// Steam sends encrypted names as base64 wrapped every 72 characters, so long paths span
    /// several lines (the "filename base64" error on Alyx). Symlink targets are encrypted the same way.

    /// A VZip (Valve's LZMA wrapper) chunk made with Python's lzma raw LZMA1 encoder.

    func testTextKeyValues() throws {
        let text = """
        "appinfo"
        {
            "common" { "name" "Half-Life: Alyx" }
            "depots"
            {
                "546561" { "manifests" { "public" { "gid" "1436897171613240821" } } }
                "branches" { "public" { "buildid" "25487405" } }
            }
        }
        """
        let kv = try KV.parseText(Data(text.utf8))
        XCTAssertEqual(kv[path: "appinfo", "common", "name"]?.string, "Half-Life: Alyx")
        XCTAssertEqual(kv[path: "appinfo", "depots", "546561", "manifests", "public", "gid"]?.uint64, 1436897171613240821)
    }

    func testBinaryKeyValues() throws {
        var d = Data([0x00]) + Data("546560".utf8) + Data([0])
        d += Data([0x00]) + Data("appids".utf8) + Data([0])
        d += Data([0x02]) + Data("0".utf8) + Data([0]) + Data([0x80, 0x55, 0x08, 0x00])
        d += Data([0x08, 0x08, 0x08])
        let kv = try KV.parseBinary(d)
        XCTAssertEqual(kv[path: "546560", "appids", "0"]?.int, 546_176)
    }

    func testJWTSubject() {
        // header.payload.signature with payload {"sub":"76561197960287930","exp":1900000000}
        let payload = Data(#"{"sub":"76561197960287930","exp":1900000000}"#.utf8).base64EncodedString()
            .replacingOccurrences(of: "=", with: "")
        let token = "eyJhbGciOiJFZERTQSJ9.\(payload).sig"
        XCTAssertEqual(SteamID.fromJWT(token), 76561197960287930)
        XCTAssertEqual(SteamID.expiry(ofJWT: token), Date(timeIntervalSince1970: 1_900_000_000))
    }

    func testCloudRootSplit() {
        let s = CloudSync.split("%GameInstall%hlvr/SAVE/quick.sav")
        XCTAssertEqual(s.root, "GameInstall")
        XCTAssertEqual(s.path, "hlvr/SAVE/quick.sav")
        XCTAssertNil(CloudSync.split("remote.vdf").root)
    }

}
