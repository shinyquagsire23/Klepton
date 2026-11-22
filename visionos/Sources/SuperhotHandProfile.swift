import simd

enum SuperhotHandProfile {
    static let profile = BareHandProfile(
        gestures: .init(
            gripCapture: .init(fingers: 3, bendLimit: 0.54, reachScale: 1.02),
            gripHold: .init(fingers: 2, bendLimit: 0.72, reachScale: 1.15),
            palmFraction: 0.58,
            gripDebounce: 0.050,
            releaseDebounce: 0.065,
            pinchOpen: 0.036,
            pinchClosed: 0.030,
            minimumTriggerPress: 0.080
        ),
        // SUPERHOT 1.738's active rigs in the Touch node's right-handed space,
        // including VRHandController's authored local translation. These joints
        // stay in place across grip animations.
        palmCalibration: .init(
            left: .init(wrist: SIMD3(-0.005777, -0.025428, 0.113628),
                        index: SIMD3(-0.030536, -0.013385, 0.023610),
                        middle: SIMD3(-0.035181, -0.039002, 0.026924),
                        little: SIMD3(-0.010226, -0.076466, 0.039732)),
            right: .init(wrist: SIMD3(0.008693, -0.024674, 0.109608),
                         index: SIMD3(0.040730, -0.011543, 0.022073),
                         middle: SIMD3(0.046043, -0.036969, 0.025821),
                         little: SIMD3(0.021518, -0.075381, 0.036469)),
            blend: 0.15
        )
    )
}
