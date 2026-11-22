import Foundation
import simd

enum SuperhotHandProfile {
    static let gestures = BareHandGestures.Profile(
        gripCapture: .init(fingers: 3, bendLimit: 0.54, reachScale: 1.02),
        gripHold: .init(fingers: 2, bendLimit: 0.72, reachScale: 1.15),
        palmFraction: 0.58,
        gripDebounce: 0.050,
        releaseDebounce: 0.065,
        pinchOpen: 0.036,
        pinchClosed: 0.030,
        minimumTriggerPress: 0.080
    )

    static let poseBlend: Float = 0.15

    struct Palm: Decodable {
        let wrist: SIMD3<Float>
        let index: SIMD3<Float>
        let middle: SIMD3<Float>
        let little: SIMD3<Float>

        var basis: simd_quatf? {
            let finger = middle - wrist
            let cross = simd_cross(finger, index - little)
            guard simd_length_squared(finger) > 0.0001,
                  simd_length_squared(cross) > 1e-8 else { return nil }
            let along = simd_normalize(finger)
            let normal = simd_normalize(cross)
            return simd_quatf(simd_float3x3(columns: (along, simd_cross(normal, along), normal)))
        }
    }

    // SUPERHOT 1.738's active rigs in the Touch node's right-handed space,
    // including VRHandController's authored local translation. These joints
    // stay in place across grip animations.
    private static let model = [
        Palm(wrist: SIMD3(-0.005777, -0.025428, 0.113628),
             index: SIMD3(-0.030536, -0.013385, 0.023610),
             middle: SIMD3(-0.035181, -0.039002, 0.026924),
             little: SIMD3(-0.010226, -0.076466, 0.039732)),
        Palm(wrist: SIMD3(0.008693, -0.024674, 0.109608),
             index: SIMD3(0.040730, -0.011543, 0.022073),
             middle: SIMD3(0.046043, -0.036969, 0.025821),
             little: SIMD3(0.021518, -0.075381, 0.036469)),
    ]

    static func touchPose(_ palm: Palm, hand: Int) -> (SIMD3<Float>, simd_quatf)? {
        let authored = model[hand]
        guard let target = palm.basis, let source = authored.basis else { return nil }
        let rotation = simd_normalize(target * source.inverse)
        return (palm.wrist - rotation.act(authored.wrist), rotation)
    }
}
