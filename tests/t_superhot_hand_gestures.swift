import Foundation
import simd

@main
enum SuperhotHandGestureTest {
    static func sample(curl count: Int, gap: Float?) -> BareHandGestures.Sample {
        func finger(_ index: Int) -> BareHandGestures.Finger {
            let x = Float(index) * 0.02
            return .init(
                base: SIMD3(x, 0, -0.075),
                middle: index < count ? SIMD3(x, -0.012, -0.097) : SIMD3(x, 0, -0.115),
                tip: index < count ? SIMD3(x, -0.037, -0.063) : SIMD3(x, 0, -0.155))
        }
        return .init(wrist: .zero, middle: finger(0), ring: finger(1),
                     little: finger(2), pinchGap: gap)
    }

    static func main() {
        func select(_ id: String, _ flags: [String: Bool] = [:]) -> BareHandProfile? {
            BareHandProfile.configured(id) { flag, fallback in flags[flag] ?? fallback }
        }

        let profile = select("superhot")!
        precondition(profile.palmCalibration == nil, "Palm calibration is opt-in")
        precondition(select("unconfigured", ["KL_BARE_HANDS": true]) == nil,
                     "An override cannot enable a target without a profile")
        precondition(select("superhot", ["KL_BARE_HANDS": false]) == nil,
                     "The generic override can disable bare-hand emulation")
        precondition(select("superhot", ["KL_SUPERHOT_BARE_HANDS": false]) == nil,
                     "The legacy SUPERHOT override still works")
        precondition(select("superhot", ["KL_SUPERHOT_ALIGN_HANDS": true])?.palmCalibration != nil,
                     "Existing SUPERHOT alignment runs remain supported")
        let calibration = select("superhot", ["KL_BARE_HAND_ALIGN": true])!.palmCalibration!
        for (hand, authored) in [calibration.left, calibration.right].enumerated() {
            let shift = SIMD3<Float>(0.05, -0.08, 0.13)
            let observed = BareHandPalmCalibration.Palm(wrist: authored.wrist + shift,
                index: authored.index + shift, middle: authored.middle + shift, little: authored.little + shift)
            let pose = calibration.touchPose(observed, hand: hand)!
            precondition(simd_length(pose.0 - shift) < 0.0001 && abs(pose.1.real) > 0.9999,
                         "The profile fits each palm in its own Touch frame")
        }

        var gestures = BareHandGestures(profile: profile.gestures)
        func check(_ time: TimeInterval, curl: Int, gap: Float?,
                   grip: Bool, trigger: Bool, _ explanation: String) {
            let actual = gestures.update(sample(curl: curl, gap: gap), at: time)
            precondition(actual.grip == grip && actual.trigger == trigger, explanation)
        }

        check(0.000, curl: 0, gap: 0.055, grip: false, trigger: false, "Open hand arms grip")
        check(0.010, curl: 2, gap: 0.055, grip: false, trigger: false, "Two fingers cannot start grip")
        check(0.030, curl: 3, gap: 0.027, grip: false, trigger: false, "Closing is debounced")
        check(0.090, curl: 3, gap: 0.027, grip: true, trigger: false, "Grabbing cannot fire a closed pinch")
        check(0.100, curl: 3, gap: 0.043, grip: true, trigger: false, "Opening the pinch arms it")
        check(0.110, curl: 2, gap: 0.027, grip: true, trigger: true, "A lighter pinch fires with grip held")
        check(0.120, curl: 1, gap: 0.044, grip: true, trigger: true, "A brief curl estimate and open pinch retain the press")
        check(0.145, curl: 2, gap: nil, grip: true, trigger: true, "A missing fingertip estimate retains the pulse")
        check(0.191, curl: 2, gap: 0.033, grip: true, trigger: true, "The hysteresis band sustains the press")
        check(0.210, curl: 2, gap: 0.043, grip: true, trigger: false, "Open pinch releases after the minimum pulse")
        check(0.230, curl: 3, gap: 0.027, grip: true, trigger: true, "A second pinch fires")
        let lost = gestures.update(nil, at: 0.231)
        precondition(!lost.grip && !lost.trigger, "Hand loss releases immediately")
        check(0.240, curl: 3, gap: 0.027, grip: false, trigger: false, "A reacquired closed hand stays neutral")
        check(0.300, curl: 0, gap: 0.055, grip: false, trigger: false, "An open hand can rearm")
        check(0.320, curl: 3, gap: 0.055, grip: false, trigger: false, "A fresh squeeze begins")
        check(0.380, curl: 3, gap: 0.055, grip: true, trigger: false, "A fresh squeeze engages")
        check(0.390, curl: 0, gap: 0.055, grip: true, trigger: false, "A transient open sample keeps grip")
        check(0.460, curl: 0, gap: 0.055, grip: false, trigger: false, "Sustained opening releases grip")
        print("PASS: profile selection and palm fitting, lighter pinch, independent grip, release and rearm")
    }
}
