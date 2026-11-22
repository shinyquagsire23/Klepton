import Foundation
import simd

/// Uses palm-relative finger curl (adapted from the Kanojo controller) and an
/// independent thumb-index pinch to produce grip and index-trigger states.
struct BareHandGestures {
    struct Profile {
        struct Curl {
            let fingers: Int
            let bendLimit: Float
            let reachScale: Float
        }

        let gripCapture: Curl
        let gripHold: Curl
        let palmFraction: Float
        let gripDebounce: TimeInterval
        let releaseDebounce: TimeInterval
        let pinchOpen: Float
        let pinchClosed: Float
        let minimumTriggerPress: TimeInterval
    }

    struct Finger: Codable {
        let base: SIMD3<Float>
        let middle: SIMD3<Float>
        let tip: SIMD3<Float>
    }

    struct Sample: Codable {
        let wrist: SIMD3<Float>
        let middle: Finger
        let ring: Finger
        let little: Finger
        let pinchGap: Float?
    }

    struct Controls {
        var grip = false
        var trigger = false
    }

    private enum Grip: Equatable { case needsOpen, open, closing(TimeInterval), held }
    private enum Pinch { case needsOpen, ready, pressed(until: TimeInterval) }
    let profile: Profile
    private var grip: Grip = .needsOpen
    private var gripReleaseSince: TimeInterval?
    private var pinch: Pinch = .needsOpen

    init(profile: Profile) {
        self.profile = profile
    }

    mutating func reset() {
        self = Self(profile: profile)
    }

    mutating func update(_ sample: Sample?, at now: TimeInterval) -> Controls {
        guard let sample, let curled = isCurled(sample) else {
            reset()
            return Controls()
        }

        let wasHeld = grip == .held
        switch grip {
        case .needsOpen:
            if !curled { grip = .open }
        case .open:
            if curled { grip = .closing(now) }
        case .closing(let since):
            if !curled { grip = .open }
            else if now - since >= profile.gripDebounce { grip = .held }
        case .held:
            if curled {
                gripReleaseSince = nil
            } else {
                let since = gripReleaseSince ?? now
                gripReleaseSince = since
                if now - since >= profile.releaseDebounce {
                    grip = .open
                    gripReleaseSince = nil
                }
            }
        }

        let held = grip == .held
        if wasHeld != held { pinch = .needsOpen }
        switch grip {
        case .open, .held:
            updatePinch(sample.pinchGap, at: now)
        case .needsOpen, .closing:
            pinch = .needsOpen
        }
        if case .pressed = pinch { return Controls(grip: held, trigger: true) }
        return Controls(grip: held)
    }

    private func isCurled(_ sample: Sample) -> Bool? {
        let axis = sample.middle.base - sample.wrist
        let length = simd_length(axis)
        guard length.isFinite, length >= 0.035 else { return nil }
        let palm = sample.wrist + profile.palmFraction * axis
        let threshold = (grip == .held || grip == .needsOpen) ? profile.gripHold : profile.gripCapture

        var curled = 0
        var observed = 0
        for finger in [sample.middle, sample.ring, sample.little] {
            let proximal = finger.middle - finger.base
            let distal = finger.tip - finger.middle
            let lengths = simd_length(proximal) * simd_length(distal)
            let bend = simd_dot(proximal, distal) / lengths
            let reach = simd_distance(finger.tip, palm)
            guard lengths > 0.000035, bend.isFinite, reach.isFinite else { continue }
            observed += 1
            if bend <= threshold.bendLimit, reach <= length * threshold.reachScale {
                curled += 1
            }
        }
        // Acquisition requires all three fingers. Once held, one finger can
        // shift or be briefly occluded as the index finger pinches.
        let required = grip == .held ? profile.gripHold.fingers : profile.gripCapture.fingers
        return observed < required ? nil : curled >= threshold.fingers
    }

    private mutating func updatePinch(_ gap: Float?, at now: TimeInterval) {
        // Keep a recognized press visible across guest frames, even if the
        // next skeleton estimate is already open or briefly unavailable.
        if case .pressed(let until) = pinch, now < until { return }
        guard let gap, gap.isFinite, gap >= 0 else {
            pinch = .needsOpen
            return
        }
        switch pinch {
        case .needsOpen, .pressed:
            if gap >= profile.pinchOpen { pinch = .ready }
        case .ready:
            if gap <= profile.pinchClosed { pinch = .pressed(until: now + profile.minimumTriggerPress) }
        }
    }
}
