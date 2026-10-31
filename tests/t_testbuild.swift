// Which builds are test builds (KleptonTestBuild.swift): the launch line's half
// of the rule, run on the Mac. Only KL_TEST_BUILD=1 makes one — not
// KLEPTON_TEST_ON, which AVP Play sets at BUILD time for a player who chose its
// experimental switches. The build-time half is tests/t_testbuild_gen.py.
// `make testbuild`.
import Foundation

@main
struct TestBuildCheck {
    static var failed = 0
    static func expect(_ env: [String: String], _ want: Bool) {
        let got = KleptonTestBuild.launched(env)
        print("  \(got == want ? "ok  " : "FAIL") \(env) -> \(got ? "test build" : "player's build")")
        if got != want { failed += 1 }
    }

    static func main() {
        print("=== test build: the launch line ===")
        expect([:], false)
        expect(["KL_TEST_BUILD": "1"], true)
        expect(["KL_TEST_BUILD": "0"], false)
        expect(["KL_TEST_BUILD": ""], false)
        expect(["KL_TEST_BUILD": "yes"], false)
        expect(["KLEPTON_TEST_ON": "KL_XR_PREDICT,KL_METALFX_UPSCALE"], false)
        expect(["KLEPTON_TEST_ON": "KL_TEST_BUILD"], false)
        expect(["KLEPTON_TEST_ON": "KL_XR_PREDICT", "KL_TEST_BUILD": "1"], true)
        if failed > 0 { print("testbuild: FAIL (\(failed))"); exit(1) }
        print("testbuild: PASS")
    }
}
