import simd

struct BareHandProfile {
    let gestures: BareHandGestures.Profile
    let palmCalibration: BareHandPalmCalibration?

    static func configured(_ id: String, flag: (String, Bool) -> Bool) -> Self? {
        let profile: Self
        let enabledByDefault: Bool
        let alignByDefault: Bool
        switch id {
        case "superhot":
            profile = SuperhotHandProfile.profile
            // Keep existing SUPERHOT launch scripts compatible.
            enabledByDefault = flag("KL_SUPERHOT_BARE_HANDS", true)
            alignByDefault = flag("KL_SUPERHOT_ALIGN_HANDS", false)
        default:
            return nil
        }
        guard flag("KL_BARE_HANDS", enabledByDefault) else { return nil }
        return Self(
            gestures: profile.gestures,
            palmCalibration: flag("KL_BARE_HAND_ALIGN", alignByDefault) ? profile.palmCalibration : nil
        )
    }
}

/// Fits the tracked palm to a profile's authored left and right Touch frames.
struct BareHandPalmCalibration {
    struct Palm {
        let wrist: SIMD3<Float>
        let index: SIMD3<Float>
        let middle: SIMD3<Float>
        let little: SIMD3<Float>

        fileprivate var basis: simd_quatf? {
            let finger = middle - wrist
            let cross = simd_cross(finger, index - little)
            guard simd_length_squared(finger) > 0.0001,
                  simd_length_squared(cross) > 1e-8 else { return nil }
            let along = simd_normalize(finger)
            let normal = simd_normalize(cross)
            return simd_quatf(simd_float3x3(columns: (along, simd_cross(normal, along), normal)))
        }
    }

    let left: Palm
    let right: Palm
    let blend: Float

    func touchPose(_ palm: Palm, hand: Int) -> (SIMD3<Float>, simd_quatf)? {
        let authored = hand == 0 ? left : right
        guard let target = palm.basis, let source = authored.basis else { return nil }
        let rotation = simd_normalize(target * source.inverse)
        return (palm.wrist - rotation.act(authored.wrist), rotation)
    }
}
