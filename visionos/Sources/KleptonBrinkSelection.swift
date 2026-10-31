import simd

// Pinch release is a tap only if the same gesture never became a drag.
struct KLBrinkSelectionGesture {
    enum Action { case tap; case swipe(SIMD2<Float>) }
    private var last = SIMD2<Float>.zero
    private var dragged = false
    mutating func update(translation: SIMD2<Float>, ended: Bool, cancelled: Bool) -> Action? {
        guard !cancelled, translation.x.isFinite, translation.y.isFinite else { return nil }
        let step = translation - last
        if simd_length(step) >= 0.035 {
            last = translation; dragged = true
            return .swipe(step)
        }
        return ended && !dragged ? .tap : nil
    }
}

// OpenXR: +Z points away from the tip, +Y is dorsal. ARKit's frames are
// not OpenXR frames. Infer a joint's dorsal local axis on an open finger and
// retain it through curling, rather than allowing the axis to flip mid-gesture.
func klConvertOpenXRHand(_ poses: inout [Float], flags: inout [UInt32],
                                hand: Int, axes: inout [Int]) -> Bool {
    guard poses.count == 208, poses.allSatisfy({ $0.isFinite }) else { return false }
    func p(_ j: Int) -> SIMD3<Float> { SIMD3(poses[j*8], poses[j*8+1], poses[j*8+2]) }
    var points = (0..<26).map { p($0) }
    points[0] = (points[11] + points[12]) * 0.5 // center of middle metacarpal BONE
    let forward = points[12] - points[1]
    let across = points[7] - points[22]
    var dorsal = simd_cross(across, forward) * (hand == 0 ? Float(1) : Float(-1))
    guard simd_length_squared(dorsal) > 1e-10 else { return false }
    dorsal = simd_normalize(dorsal)
    let next = [12,11,3,4,5,5,7,8,9,10,10,12,13,14,15,15,17,18,19,20,20,22,23,24,25,25]
    let tips: Set<Int> = [5,10,15,20,25]
    var rotations = [simd_quatf]()
    for j in 0..<26 {
        var backward = tips.contains(j) ? points[j-1] - points[j] : points[j] - points[next[j]]
        guard simd_length_squared(backward) > 1e-10 else { return false }
        backward = simd_normalize(backward)
        let source = tips.contains(j) ? j-1 : j
        let k = source * 8
        let raw = simd_quatf(ix: poses[k+3], iy: poses[k+4], iz: poses[k+5], r: poses[k+6])
        guard simd_length_squared(raw.vector) > 0.5 else { return false }
        let basis = simd_float3x3(simd_normalize(raw))
        if axes[j] < 0 && abs(simd_dot(backward, dorsal)) < 0.3 {
            var score: Float = 0
            for a in 0..<3 {
                let d = simd_dot(basis[a], dorsal)
                if abs(d) > score { score = abs(d); axes[j] = a*2 + (d < 0 ? 1 : 0) }
            }
        }
        var up = dorsal
        if j > 1 && axes[j] >= 0 {
            up = basis[axes[j]/2] * (axes[j]%2 == 0 ? Float(1) : Float(-1))
        }
        up -= backward * simd_dot(up, backward)
        if simd_length_squared(up) < 1e-8 {
            // Still no reliable frame: do not advertise an active malformed skeleton.
            return false
        }
        up = simd_normalize(up)
        let right = simd_normalize(simd_cross(up, backward))
        up = simd_cross(backward, right)
        rotations.append(simd_normalize(simd_quatf(simd_float3x3(columns: (right, up, backward)))))
    }
    for j in 0..<26 {
        let k = j*8, q = rotations[j]
        poses[k] = points[j].x; poses[k+1] = points[j].y; poses[k+2] = points[j].z
        poses[k+3] = q.imag.x; poses[k+4] = q.imag.y; poses[k+5] = q.imag.z; poses[k+6] = q.real
    }
    poses[7] = 0.015
    flags[0] = 3 | (flags[11] & flags[12] & 12)
    return true
}
