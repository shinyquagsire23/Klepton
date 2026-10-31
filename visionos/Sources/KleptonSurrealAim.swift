import Foundation
import simd

/// User offsets are optional, per hand, and applied AFTER forward alignment.
struct SurrealAimOffset: Equatable {
    var degrees = SIMD3<Float>.zero
    static func key(_ hand: Int) -> String { "klepton.surreal.\(hand).aimDegrees" }
    static func stored(hand: Int, defaults: UserDefaults = .standard) -> Self {
        guard let values = defaults.array(forKey: key(hand)) as? [Double], values.count == 3,
              values.allSatisfy(\.isFinite) else { return Self() }
        return Self(degrees: SIMD3(values.map { Float(min(90, max(-90, $0))) }))
    }
    func save(hand: Int, defaults: UserDefaults = .standard) {
        defaults.set((0..<3).map { Double(degrees[$0].isFinite ? min(90, max(-90, degrees[$0])) : 0) }, forKey: Self.key(hand))
    }
    var rotation: simd_quatf {
        let r = degrees * (.pi / 180)
        return (simd_quatf(angle: r.x, axis: [1, 0, 0])
            * simd_quatf(angle: r.y, axis: [0, 1, 0])
            * simd_quatf(angle: r.z, axis: [0, 0, 1])).normalized
    }
}

/// Fixed sensor-to-world alignment. No hand rotation enters this path.
struct SurrealControllerOrientation {
    var world: simd_quatf
    init(device: simd_quatf, target: simd_quatf) {
        world = (target * device.inverse).normalized
    }
    func rotation(device: simd_quatf, offset: simd_quatf) -> simd_quatf {
        (world * device * offset).normalized
    }
    static func angle(_ a: simd_quatf, _ b: simd_quatf) -> Float {
        2 * acos(min(1, abs(simd_dot(a.normalized.vector, b.normalized.vector))))
    }
    /// Level heading for the explicit forward hold; never a continuous correction.
    static func forward(head: simd_float4x4) -> simd_quatf? {
        let direction = SIMD3<Float>(-head.columns.2.x, 0, -head.columns.2.z)
        guard (0..<3).allSatisfy({ direction[$0].isFinite }), simd_length(direction) > 0.1 else { return nil }
        let forward = simd_normalize(direction)
        let right = simd_cross(forward, SIMD3<Float>(0, 1, 0))
        return simd_quatf(simd_float3x3(columns: (right, [0, 1, 0], -forward))).normalized
    }
}
